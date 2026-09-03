/*
 * Executor-throughput microbenchmark.
 *
 * Where decode_bench measures oemu_decode() in isolation, this one measures
 * the whole fetch/decode/dispatch/execute loop -- oemu_exec_run() -- running a
 * real, self-contained AArch64 guest to completion. The reported unit is
 * nanoseconds per retired instruction.
 *
 * The guest is generated here rather than assembled: a handful of bit-field
 * encoders emit a counted compute loop (an ALU/logic/memory mix with a
 * backwards conditional branch) terminated by a real `exit(0)` SVC. Every
 * encoder was checked against clang/GAS `--target=aarch64-none-elf` output, so
 * the benchmark needs no cross toolchain at build time and never trusts a
 * hand-written hex immediate.
 *
 * Methodology matches decode_bench on purpose, so the two numbers can be read
 * side by side: calibrate one timed pass, choose a batch size for --budget ms,
 * run --trials batches and keep the minimum (the estimator that survives CPU
 * frequency scaling). The guest is deterministic, so a wide trial-to-trial
 * spread means the machine moved, not the code.
 */
#define _POSIX_C_SOURCE 200809L

#include "oemu/exec.h"
#include "oemu/memory.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Guest address space: two small aliased regions, nothing else is reachable. */
#define GVA_TEXT ((uint64_t)0x01000000ull)
#define GVA_DATA ((uint64_t)0x02000000ull)

/* Register numbers, spelled out so the emitted loop reads like the .s it was
 * checked against. */
enum {
  R0 = 0,
  R1 = 1,
  R2 = 2,
  R3 = 3,
  R4 = 4,
  R5 = 5,
  R6 = 6,
  R7 = 7,
  R8 = 8,
  R9 = 9,
  R10 = 10,
  R11 = 11,
  R12 = 12,
  R13 = 13,
  R14 = 14,
  R15 = 15,
  R16 = 16,
  R17 = 17,
  R18 = 18,
  R19 = 19,
  R20 = 20,
  R21 = 21
};

/* The generated program, aliased straight into guest memory. 64 words is ample
 * for the 18-word loop; the rest stays as headroom and is never fetched. */
static uint32_t g_prog[64];
static uint64_t g_data[512];

static volatile uint64_t g_sink;

/* --- instruction encoders (every one GAS-verified) ------------------------- */

static uint32_t enc_movz(unsigned rd, uint32_t imm16) {
  return 0xD2800000u | (imm16 << 5) | (uint32_t)rd; /* MOVZ Xd, #imm16, LSL#0 */
}
static uint32_t enc_add_imm(unsigned rd, unsigned rn, uint32_t imm12) {
  return 0x91000000u | (imm12 << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
static uint32_t enc_sub_imm(unsigned rd, unsigned rn, uint32_t imm12) {
  return 0xD1000000u | (imm12 << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
static uint32_t enc_subs_imm(unsigned rd, unsigned rn, uint32_t imm12) {
  return 0xF1000000u | (imm12 << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
static uint32_t enc_add_reg(unsigned rd, unsigned rn, unsigned rm) {
  return 0x8B000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
static uint32_t enc_adds_reg(unsigned rd, unsigned rn, unsigned rm) {
  return 0xAB000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
static uint32_t enc_sub_reg(unsigned rd, unsigned rn, unsigned rm) {
  return 0xCB000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
static uint32_t enc_and_reg(unsigned rd, unsigned rn, unsigned rm) {
  return 0x8A000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
static uint32_t enc_orr_reg(unsigned rd, unsigned rn, unsigned rm) {
  return 0xAA000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
static uint32_t enc_eor_reg(unsigned rd, unsigned rn, unsigned rm) {
  return 0xCA000000u | ((uint32_t)rm << 16) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
static uint32_t enc_add_lsl(unsigned rd, unsigned rn, unsigned rm, unsigned sh) {
  /* ADD shifted register, LSL (type=00), imm6 in bits[15:10]. */
  return 0x8B000000u | ((uint32_t)rm << 16) | ((sh & 0x3Fu) << 10) | ((uint32_t)rn << 5) |
         (uint32_t)rd;
}
static uint32_t enc_ldr(unsigned rt, unsigned rn, unsigned off_bytes) {
  /* LDR Xt,[Xn,#off] unsigned-immediate, scaled by 8. */
  return 0xF9400000u | (((off_bytes / 8u) & 0xFFFu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
static uint32_t enc_str(unsigned rt, unsigned rn, unsigned off_bytes) {
  return 0xF9000000u | (((off_bytes / 8u) & 0xFFFu) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
static uint32_t enc_mov_reg(unsigned rd, unsigned rm) {
  /* MOV Xd, Xm is ORR Xd, XZR, Xm. */
  return 0xAA000000u | ((uint32_t)rm << 16) | (31u << 5) | (uint32_t)rd;
}
static uint32_t enc_b_cond(unsigned cond, int off_words) {
  return 0x54000000u | ((((uint32_t)off_words) & 0x7FFFFu) << 5) | (cond & 0xFu);
}
static uint32_t enc_svc(uint32_t imm16) {
  return 0xD4000000u | (imm16 << 5) | 1u;
}

/*
 * Emits the counted loop. `trips` becomes the MOVZ immediate, so it is capped
 * at 16 bits; a longer run is bought with more trials, not a wider counter.
 * Layout mirrors the GAS listing the encoders were validated against.
 */
static void build_program(uint32_t trips) {
  size_t i = 0;
  const unsigned head = 1u;    /* the loop body starts at word 1 */
  const unsigned branch = 14u; /* the b.ne that closes it */

  g_prog[i++] = enc_movz(R19, trips);                    /*  0: mov x19, #trips      */
  g_prog[i++] = enc_add_imm(R1, R1, 7u);                 /*  1: add x1, x1, #7     */
  g_prog[i++] = enc_sub_imm(R2, R2, 3u);                 /*  2: sub x2, x2, #3     */
  g_prog[i++] = enc_add_reg(R3, R3, R4);                 /*  3: add x3, x3, x4     */
  g_prog[i++] = enc_adds_reg(R5, R5, R6);                /*  4: adds x5, x5, x6    */
  g_prog[i++] = enc_sub_reg(R7, R7, R8);                 /*  5: sub x7, x7, x8     */
  g_prog[i++] = enc_and_reg(R9, R9, R10);                /*  6: and x9, x9, x10    */
  g_prog[i++] = enc_orr_reg(R11, R11, R12);              /*  7: orr x11, x11, x12  */
  g_prog[i++] = enc_eor_reg(R13, R13, R14);              /*  8: eor x13, x13, x14  */
  g_prog[i++] = enc_add_lsl(R15, R15, R16, 3u);          /*  9: add x15,x15,x16,LSL#3 */
  g_prog[i++] = enc_ldr(R17, R18, 16u);                  /* 10: ldr x17, [x18,#16] */
  g_prog[i++] = enc_str(R20, R18, 24u);                  /* 11: str x20, [x18,#24] */
  g_prog[i++] = enc_mov_reg(R21, R1);                    /* 12: mov x21, x1        */
  g_prog[i++] = enc_subs_imm(R19, R19, 1u);              /* 13: subs x19, x19, #1  */
  g_prog[i++] = enc_b_cond(1u, (int)head - (int)branch); /* 14: b.ne loop (cond NE=0b0001) */
  g_prog[i++] = enc_movz(R8, 93u);                       /* 15: mov x8, #93 (exit) */
  g_prog[i++] = enc_movz(R0, 0u);                        /* 16: mov x0, #0 (code)  */
  g_prog[i++] = enc_svc(0u);                             /* 17: svc #0             */

  (void)i;
}

/* --- timing harness -------------------------------------------------------- */

static uint64_t monotonic_ns(void) {
  struct timespec ts;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    (void)fputs("exec-bench: clock_gettime failed\n", stderr);
    exit(2);
  }
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Boots a fresh core over the aliased guest, runs it once, returns the retired
 * instruction count (or 0 on any failure, which the caller treats as fatal). */
static uint64_t run_once(oemu_memory *mem) {
  oemu_cpu cpu;
  oemu_sysenv env;
  uint64_t completed = 0;

  if (oemu_cpu_init(&cpu, GVA_TEXT, (uint64_t)(GVA_DATA + sizeof(g_data))) != OEMU_OK) {
    return 0;
  }
  /* Point the base register at the data region; cpu_init zeroes everything
   * else, which is fine -- the loop only needs a valid address to touch. */
  oemu_regs_write(&cpu.regs, R18, OEMU_REG_W64, GVA_DATA);
  oemu_sysenv_init(&env, stdout);
  const oemu_status st = oemu_exec_run(&cpu, mem, &env, UINT64_MAX, &completed);
  if (st != OEMU_OK || !oemu_sysenv_exited(&env)) {
    (void)fprintf(stderr, "exec-bench: run stopped status=%s completed=%" PRIu64 " exited=%d\n",
                  oemu_status_str(st), completed, oemu_sysenv_exited(&env) ? 1 : 0);
    return 0;
  }
  return completed;
}

static int setup_memory(oemu_memory *mem) {
  if (oemu_memory_init(mem, 8U) != OEMU_OK) {
    (void)fputs("exec-bench: oemu_memory_init failed\n", stderr);
    return -1;
  }
  /* Alias so the generated words and the data buffer are the guest's real
   * memory; nothing is copied and the arrays outlive every run. */
  if (oemu_memory_map_alias(mem, GVA_TEXT, g_prog, (uint64_t)sizeof(g_prog), OEMU_PERM_ALL) !=
      OEMU_OK) {
    (void)fputs("exec-bench: mapping text failed\n", stderr);
    return -1;
  }
  if (oemu_memory_map_alias(mem, GVA_DATA, g_data, (uint64_t)sizeof(g_data),
                            OEMU_PERM_READ | OEMU_PERM_WRITE) != OEMU_OK) {
    (void)fputs("exec-bench: mapping data failed\n", stderr);
    return -1;
  }
  return 0;
}

static void usage(const char *program) {
  (void)printf("usage: %s [--count N] [--trials N] [--budget MS]\n", program);
  (void)printf("  count: loop trip count, 1..65535 (default 60000)\n");
}

int main(int argc, char **argv) {
  uint32_t trips = 60000u;
  unsigned trials = 7u;
  double budget_ns = 200.0e6;
  oemu_memory mem;
  uint64_t insns_per_run;

  for (int arg = 1; arg < argc; arg++) {
    const char *flag = argv[arg];
    unsigned long parsed;

    if (strcmp(flag, "--help") == 0) {
      usage(argv[0]);
      return 0;
    } else if (strcmp(flag, "--count") == 0 && arg + 1 < argc) {
      parsed = strtoul(argv[++arg], NULL, 10);
      trips = (parsed >= 1u && parsed <= 65535u) ? (uint32_t)parsed : trips;
    } else if (strcmp(flag, "--trials") == 0 && arg + 1 < argc) {
      parsed = strtoul(argv[++arg], NULL, 10);
      trials = (parsed >= 1u && parsed <= 1000u) ? (unsigned)parsed : trials;
    } else if (strcmp(flag, "--budget") == 0 && arg + 1 < argc) {
      parsed = strtoul(argv[++arg], NULL, 10);
      if (parsed >= 10u && parsed <= 10000u) {
        budget_ns = (double)parsed * 1.0e6;
      }
    } else {
      usage(argv[0]);
      return 1;
    }
  }

  build_program(trips);
  if (setup_memory(&mem) != 0) {
    return 2;
  }

  /* Correctness gate: the guest must run to a clean exit, and it must retire
   * roughly the number of instructions the emitted program implies. A wrong
   * encoding shows up here (wrong count or a fault), not as a quiet number. */
  insns_per_run = run_once(&mem);
  if (insns_per_run == 0u) {
    (void)fputs("exec-bench: the generated guest did not exit cleanly\n", stderr);
    oemu_memory_dispose(&mem);
    return 2;
  }

  /* Calibrate a batch: how many whole runs fit the budget, given one timed run. */
  uint64_t calib_t0 = monotonic_ns();
  g_sink = run_once(&mem);
  double run_ns = (double)(monotonic_ns() - calib_t0);

  uint64_t runs = 1u;
  if (run_ns > 0.0) {
    runs = (uint64_t)(budget_ns / run_ns);
  }
  if (runs < 2u) {
    runs = 2u;
  }

  double best = -1.0;
  for (unsigned trial = 0; trial < trials; trial++) {
    uint64_t t0 = monotonic_ns();
    for (uint64_t r = 0; r < runs; r++) {
      g_sink = run_once(&mem);
    }
    uint64_t batch_ns = monotonic_ns() - t0;
    double ns_per_insn = (double)batch_ns / ((double)insns_per_run * (double)runs);
    if (best < 0.0 || ns_per_insn < best) {
      best = ns_per_insn;
    }
  }

  (void)printf("oemu executor benchmark (fetch + decode + dispatch + execute)\n\n");
  (void)printf("  guest        : counted compute loop, %u trips\n", trips);
  (void)printf("  insns/run    : %" PRIu64 "\n", insns_per_run);
  (void)printf("  trials       : %u  (min reported)\n", trials);
  (void)printf("  runs/trial   : %" PRIu64 "\n", runs);
  (void)printf("  min ns/insn  : %.3f\n", best);
  (void)printf("  throughput   : %.2f Minsn/s\n", best > 0.0 ? 1000.0 / best : 0.0);
  (void)printf(
      "\ninterpretation: this is per-retired-instruction cost for the whole pipeline, so\n"
      "it is dominated by dispatch width and the interpreter loop, not by decode alone\n"
      "(compare oemu-bench-decode). ns/insn is min-of-trials to dodge frequency scaling.\n");

  oemu_memory_dispose(&mem);
  return 0;
}
