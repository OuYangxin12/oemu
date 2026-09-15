/*
 * bench/perf/oemu-perf.c -- an in-guest CPU benchmark suite for the boot path.
 *
 * The interpreter-level benchmarks (bench/c) measure the executor against a
 * corpus blob; the gate (scripts/boot-linux-gate.sh) measures wall-to-shell.
 * Neither answers the question a user actually asks: what does this emulator
 * feel like *under an OS*, running programs a normal program looks like.
 * This binary answers that one, on both sides of the oracle discipline: the
 * same ELF, booted by the same kernel Image in the same initramfs, runs every
 * test to completion and powers the machine down under both QEMU (TCG, the
 * oracle) and oemu (the emulator under test).
 *
 * The suite is table-driven and deliberately shares its workload source with
 * the decoder corpus: the ten k_* kernels under bench/corpus/ are real
 * compiler output chosen to stress one instruction family each (addsub,
 * logic, muldiv, bitfield, csel, branches, movewide, ldst, hash, memops), so
 * the two benchmark layers -- host-side blob and in-guest ELF -- measure the
 * same work at different layers of the same project. On top of those,
 * hand-written phases add the pressures a table of kernels cannot express:
 * pure dependency latency, a 2 MiB working set, call overhead, stack churn,
 * the CRC32 system instructions, and a jump table.
 *
 * Constraints the project itself wrote down the hard way:
 *  - INT ONLY. oemu implements the integer subset (FP/SIMD arithmetic is
 *    issue #30 / M6 scope), so every test is pure A64 integer code and the
 *    build script refuses to pack the image unless the cross objdump finds
 *    zero mnemonics outside its whitelist. The guest is freestanding with no
 *    printf: glibc's stdio drags NEON in no matter which flags you pass
 *    (learned building the busybox fixture), so the output path is a
 *    hand-rolled fixed-point writer over write(2).
 *  - THE CLOCK IS THE KERNEL'S. Timing is clock_gettime(CLOCK_MONOTONIC),
 *    which both sides answer from their own arch timer, so the numbers
 *    compare emulators, not userspace clocks. Each test prints its own
 *    duration in nanoseconds and milliseconds; the suite prints a total.
 *  - THE REBOOT ABI IS THE BOOTED TREE'S. The magics below are copied from
 *    the tree under guest/, header include/uapi/linux/reboot.h, exactly as
 *    tests/guest/init.c learned to do the hard way.
 *
 * Determinism: fixed iteration counts, static ET_EXEC (no PIE to slide), no
 * randomness -- both sides retire the same instruction stream, and every
 * line's `r=` checksum lets a mis-emulated instruction show up as a diverent
 * checksum rather than as merely "slower".
 *
 * Output format, one line per test (scripts/bench-report.sh parses it):
 *   PERF <name> ns=<nanoseconds> ms=<fractional-millis> ops=<iterations> r=<checksum>
 * plus PERF TOTAL ms=... tests=<n> and the start/end/power markers.
 */

typedef unsigned long size_t;
typedef unsigned long u64;
typedef long i64;
typedef unsigned int u32;
typedef unsigned char u8;

/* --- raw syscalls (same spelling as tests/guest/init.c) --------------------- */

#define SYS_clock_gettime 113
#define SYS_write         64
#define SYS_reboot        142
#define SYS_exit_group    94

#define CLOCK_MONOTONIC 1

#define REBOOT_MAGIC1        0xfee1dead
#define REBOOT_MAGIC2        672274793 /* this tree's LINUX_REBOOT_MAGIC2 */
#define REBOOT_CMD_POWER_OFF 0x4321FEDC

#define SVC_CLOBBERS "x4", "x5", "x6", "x7", "memory"

static long sys4(long n, long a, long b, long c, long d) {
  register long x8 __asm__("x8") = n;
  register long x0 __asm__("x0") = a;
  register long x1 __asm__("x1") = b;
  register long x2 __asm__("x2") = c;
  register long x3 __asm__("x3") = d;
  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : SVC_CLOBBERS);
  return x0;
}

static long sys3(long n, long a, long b, long c) {
  return sys4(n, a, b, c, 0);
}

struct timespec64 {
  i64 tv_sec;
  u64 tv_nsec;
};

static u64 now_ns(void) {
  struct timespec64 ts;
  ts.tv_sec = 0;
  ts.tv_nsec = 0;
  (void)sys3(SYS_clock_gettime, CLOCK_MONOTONIC, (long)&ts, 0);
  return (u64)ts.tv_sec * 1000000000UL + (u64)ts.tv_nsec;
}

/* --- output: write(2) plus a decimal writer; no printf exists here ---------- */

static size_t slen(const char *s) {
  size_t n = 0;
  while (s[n] != '\0') {
    n++;
  }
  return n;
}

static void say(const char *s) {
  (void)sys3(SYS_write, 1, (long)s, (long)slen(s));
}

static void put_u64(u64 v, char *out) {
  char tmp[24];
  size_t i = 0;
  size_t j = 0;
  do {
    tmp[i++] = (char)('0' + (v % 10UL));
    v /= 10UL;
  } while ((v != 0UL) && (i < sizeof(tmp)));
  while (i > 0UL) {
    out[j++] = tmp[--i];
  }
  out[j] = '\0';
}

static void emit_u64(u64 v) {
  char num[24];
  put_u64(v, num);
  (void)sys3(SYS_write, 1, (long)num, (long)slen(num));
}

/* `123.456` -- milliseconds with microsecond resolution, fixed point. */
static void emit_ms(u64 ns) {
  char num[24];
  put_u64(ns / 1000000UL, num);
  (void)sys3(SYS_write, 1, (long)num, (long)slen(num));
  say(".");
  const u64 frac = (ns / 1000UL) % 1000UL;
  char f[4] = {'0' + (char)(frac / 100UL), '0' + (char)((frac / 10UL) % 10UL),
               '0' + (char)(frac % 10UL), '\0'};
  (void)sys3(SYS_write, 1, (long)f, 3);
}

/* --- the arena: one BSS block serves the memory-hungry tests ---------------- */

#define MEM_WORDS  (2UL * 1024UL * 1024UL / 8UL) /* 256 Ki u64 = 2 MiB        */
#define SMALL_MASK (64UL * 1024UL / 8UL - 1UL)   /* 64 Ki u64 = 512 KiB slice */

static u64 arena[MEM_WORDS];
static u64 arena_src[MEM_WORDS];

/* --- corpus kernels (bench/corpus/k_*.c): same sources as the decode blobs -- */

u64 oemu_k_addsub(u64 seed);
u64 oemu_k_logic(u64 seed, u64 key);
u64 oemu_k_muldiv(u64 seed, u64 divisor);
u64 oemu_k_bitfield(u64 seed);
u64 oemu_k_csel(u64 seed);
u64 oemu_k_branches(u64 seed);
u64 oemu_k_movewide(u64 seed);
u64 oemu_k_hash(u64 seed, const u8 *bytes, size_t n);
u64 oemu_k_memops(u8 *dst, const u8 *src, size_t n);

static u64 run_addsub(u64 iters) {
  u64 r = 0;
  for (u64 i = 0; i < iters; i++) {
    r ^= oemu_k_addsub(i + 0x100000001UL);
  }
  return r;
}

static u64 run_logic(u64 iters) {
  u64 r = 0;
  for (u64 i = 0; i < iters; i++) {
    r ^= oemu_k_logic(i, 0x9e3779b97f4a7c15UL);
  }
  return r;
}

static u64 run_muldiv(u64 iters) {
  u64 r = 0;
  for (u64 i = 0; i < iters; i++) {
    r ^= oemu_k_muldiv(i + 1UL, (i * 1664525UL) | 1UL);
  }
  return r;
}

static u64 run_bitfield(u64 iters) {
  u64 r = 0;
  for (u64 i = 0; i < iters; i++) {
    r ^= oemu_k_bitfield(i + 0x9e3779b9UL);
  }
  return r;
}

static u64 run_csel(u64 iters) {
  u64 r = 0;
  for (u64 i = 0; i < iters; i++) {
    r ^= oemu_k_csel(i + 0xcafebabeUL);
  }
  return r;
}

static u64 run_branches(u64 iters) {
  u64 r = 0;
  for (u64 i = 0; i < iters; i++) {
    r ^= oemu_k_branches(i + 0xdeadbeefUL);
  }
  return r;
}

static u64 run_movewide(u64 iters) {
  u64 r = 0;
  for (u64 i = 0; i < iters; i++) {
    r ^= oemu_k_movewide(i + 0x85ebca6bUL);
  }
  return r;
}

static u64 run_hash(u64 iters) {
  u64 r = 0;
  for (u64 i = 0; i < iters; i++) {
    r ^= oemu_k_hash(i, (const u8 *)arena, 256UL);
  }
  return r;
}

static u64 run_memops(u64 iters) {
  u64 r = 0;
  for (u64 i = 0; i < iters; i++) {
    r ^= oemu_k_memops((u8 *)arena, (const u8 *)arena_src, 4096UL);
  }
  return r;
}

/* --- hand-written phases: the pressures a kernel table cannot express ------- */

/* Pure dependency latency: every step waits for the last, so the number is
 * the serial cost per op -- distinct from the throughput-shaped k_addsub. */
static u64 phase_add_chain(u64 iters) {
  u64 a = 0x123456789abcdefUL;
  u64 b = 0xfedcba9876543210UL;
  for (u64 i = 0; i < iters; i++) {
    a += b;
    b ^= a >> 7;
    a = (a << 3) | (a >> 61);
    b -= a & 0xffffUL;
  }
  return a ^ b;
}

static u64 phase_mul_chain(u64 iters) {
  u64 a = 0x9e3779b97f4a7c15UL;
  u64 c = 0x0123456789abcdefUL;
  for (u64 i = 0; i < iters; i++) {
    a = a * c;
    c ^= a >> 32;
  }
  return a + c;
}

/* Runtime divisors: real divisions (the k_muldiv constant forms lower to
 * reciprocal multiply; this one keeps the divider hot). */
static u64 phase_div_chain(u64 iters) {
  u64 a = 0xf00df00df00df00dUL;
  u64 r = 0;
  for (u64 i = 0; i < iters; i++) {
    const u64 d = (a | 1UL) >> 32; /* runtime, >= 1, varies per step */
    r += a / d;
    r += (u64)((i64)(a ^ r) / (i64)(d | 0x10000UL));
    a = r * 0x2545f4914f6cdd1dUL + i;
  }
  return a ^ r;
}

/* Small working set (512 KiB): dcache-resident pointer chase with RMW. */
static u64 phase_mem_512k(u64 iters) {
  u64 sum = 0;
  u64 idx = 7;
  for (u64 i = 0; i < iters; i++) {
    idx = (idx * 33UL) & SMALL_MASK;
    const u64 v = arena[idx] + 0x9e3779b97f4a7c15UL;
    arena[idx] = v;
    sum += v;
  }
  return sum;
}

/* Full 2 MiB working set: spills out of the L2 of plausible hosts. */
static u64 phase_mem_2m(u64 iters) {
  u64 sum = 0;
  u64 idx = 104729UL; /* coprime-ish walk, no aliasing on the low bits */
  for (u64 i = 0; i < iters; i++) {
    idx = (idx * 33UL) & (MEM_WORDS - 1UL);
    const u64 v = arena[idx] + i;
    arena[idx] = v;
    sum += v;
  }
  return sum;
}

/* Straight-line streaming copy, home-rolled so it is our loop and our ops. */
static u64 phase_memcpy(u64 iters) {
  u64 sum = 0;
  for (u64 i = 0; i < iters; i++) {
    for (u64 w = 0; w < MEM_WORDS; w++) {
      arena[w] = arena_src[w];
    }
    sum += arena[i & (MEM_WORDS - 1UL)];
  }
  return sum;
}

static u64 phase_memset(u64 iters) {
  u64 v = 0x0123456789abcdefUL;
  for (u64 i = 0; i < iters; i++) {
    v = v * 0x2545f4914f6cdd1dUL + 0x26UL;
    for (u64 w = 0; w < MEM_WORDS; w++) {
      arena[w] = v;
    }
  }
  return arena[0] + v;
}

/* Function-call overhead: eight-deep dependency chains of real calls. */
static u64 call_leaf(u64 x) {
  return x + 0x9e3779b9UL;
}
static u64 call3(u64 x) {
  return call_leaf(x ^ 11UL) + call_leaf(x ^ 22UL);
}
static u64 call2(u64 x) {
  return call3(x ^ 33UL) + call3(x ^ 44UL);
}
static u64 call1(u64 x) {
  return call2(x ^ 55UL) + call2(x ^ 66UL);
}

static u64 phase_call(u64 iters) {
  u64 r = 0;
  for (u64 i = 0; i < iters; i++) {
    r += call1(i);
  }
  return r;
}

/* Stack pressure: recursion with a real frame and a frame-resident buffer. */
static u64 stack_recurse(u64 depth, u64 seed) {
  u64 frame[16];
  for (unsigned w = 0; w < 16U; w++) {
    frame[w] = seed + w;
  }
  if (depth == 0UL) {
    u64 s = 0;
    for (unsigned w = 0; w < 16U; w++) {
      s += frame[w];
    }
    return s;
  }
  return stack_recurse(depth - 1UL, frame[depth & 15UL] ^ seed) + frame[0];
}

static u64 phase_stack(u64 iters) {
  u64 r = 0;
  for (u64 i = 0; i < iters; i++) {
    r += stack_recurse(16UL, i + 1UL);
  }
  return r;
}

/* The CRC32 system instructions (FEAT_CRC32): the kernel itself uses them,
 * and oemu decodes them -- spelled as inline asm so the instruction is ours,
 * not a compiler choice. The instruction is W-register only. */
static u64 crc32cb(u64 crc, u64 v) {
  u32 r;
  __asm__("crc32cb %w0, %w1, %w2" : "=r"(r) : "r"((u32)crc), "r"((u32)v));
  return r;
}

static u64 phase_crc32(u64 iters) {
  u64 c = 0xffffffffUL;
  for (u64 i = 0; i < iters; i++) {
    c = crc32cb(c, i ^ c);
  }
  return c;
}

/* A 16-way switch: gcc lowers this to a jump table, exercising indirect
 * branches (br), which none of the corpus kernels leans on. */
static u64 phase_table(u64 iters) {
  u64 acc = 0;
  u64 s = 0xabcdef0123456789UL;
  for (u64 i = 0; i < iters; i++) {
    s = s * 6364136223846793005UL + 1442695040888963407UL;
    switch ((s >> 33) & 0xfUL) {
      case 0:
        acc += s;
        break;
      case 1:
        acc -= s;
        break;
      case 2:
        acc ^= s << 1;
        break;
      case 3:
        acc |= s >> 3;
        break;
      case 4:
        acc &= s | 0x55UL;
        break;
      case 5:
        acc = (acc << 7) | (s & 0x7fUL);
        break;
      case 6:
        acc += ~s;
        break;
      case 7:
        acc ^= s + i;
        break;
      case 8:
        acc -= (s >> 16);
        break;
      case 9:
        acc += (s & 0xffUL) * 33UL;
        break;
      case 10:
        acc = (acc << 1) ^ (s & 1UL);
        break;
      case 11:
        acc -= i * 0x1000003UL;
        break;
      case 12:
        acc ^= (i << 8) | (s & 0xffUL);
        break;
      case 13:
        acc += (s >> 8) ^ (s << 8);
        break;
      case 14:
        acc = (acc >> 2) + s;
        break;
      default:
        acc ^= (acc >> 32) + s;
        break;
    }
  }
  return acc;
}

/* --- the table: every test is one row; adding a row adds a line of output --- */

struct test {
  const char *name;
  u64 (*fn)(u64);
  u64 iters;
};

static const struct test tests[] = {
    /* corpus kernels: one counted call per op */
    {"addsub", run_addsub, 200000UL},
    {"logic", run_logic, 200000UL},
    {"muldiv", run_muldiv, 20000UL},
    {"bitfield", run_bitfield, 200000UL},
    {"csel", run_csel, 200000UL},
    {"branches", run_branches, 200000UL},
    {"movewide", run_movewide, 200000UL},
    /* ldst is deliberately absent: k_ldst's packed struct makes unaligned
     * loads its core case, and QEMU's cortex-a53 model (ID_AA64MMFR0 = 0: the
     * hardware fixes misalignment up) never takes the SCTLR.SA0 route oemu's
     * strict read path does. Until the alignment-policy question is settled
     * as its own change, the suite stays on kernels both oracles agree on. */
    {"hash", run_hash, 30000UL},
    {"memops", run_memops, 5000UL},
    /* hand-written phases: one counted iteration per op */
    {"add-chain", phase_add_chain, 10000000UL},
    {"mul-chain", phase_mul_chain, 3000000UL},
    {"div-chain", phase_div_chain, 1000000UL},
    {"mem-512k", phase_mem_512k, 1000000UL},
    {"mem-2m", phase_mem_2m, 2000000UL},
    {"memcpy-2m", phase_memcpy, 100UL},
    {"memset-2m", phase_memset, 1000UL},
    {"call", phase_call, 1000000UL},
    {"stack", phase_stack, 100000UL},
    {"crc32", phase_crc32, 10000000UL},
    {"table", phase_table, 10000000UL},
};

/* --- driver ----------------------------------------------------------------- */

static void seed_arena(void) {
  u64 v = 0x2545f4914f6cdd1dUL;
  for (u64 i = 0; i < MEM_WORDS; i++) {
    v ^= v << 13;
    v ^= v >> 7;
    v ^= v << 17;
    arena[i] = v;
    arena_src[i] = v ^ 0xa5a5a5a5a5a5a5a5UL;
  }
}

static void run_test(const struct test *t, u64 *total_ns) {
  const u64 t0 = now_ns();
  const u64 r = t->fn(t->iters);
  const u64 t1 = now_ns();
  const u64 ns = t1 - t0;
  *total_ns += ns;
  say("PERF ");
  say(t->name);
  say(" ns=");
  emit_u64(ns);
  say(" ms=");
  emit_ms(ns);
  say(" ops=");
  emit_u64(t->iters);
  say(" r=");
  emit_u64(r);
  say("\n");
}

void _start(void);

void _start(void) {
  say("PERF-BENCH-START\n");
  seed_arena();

  u64 total = 0;
  const size_t n = sizeof(tests) / sizeof(tests[0]);
  for (size_t i = 0; i < n; i++) {
    run_test(&tests[i], &total);
  }

  say("PERF TOTAL ms=");
  emit_ms(total);
  say(" tests=");
  emit_u64(n);
  say("\n");

  say("PERF-BENCH-END\n");
  say("reboot: Power down\n");
  (void)sys4(SYS_reboot, REBOOT_MAGIC1, REBOOT_MAGIC2, REBOOT_CMD_POWER_OFF, 0);
  /* A refused reboot must not hang the harness: exit loudly instead. */
  say("PERF-BENCH-REBOOT-REFUSED\n");
  (void)sys3(SYS_exit_group, 1, 0, 0);
  for (;;) {
    /* unreachable */
  }
}
