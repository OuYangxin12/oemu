/*
 * The oemu command line.
 *
 *   oemu run <image.elf> [--max-insns N]
 *   oemu boot -kernel <Image> [-append <cmdline>] [-m MiB] [-dtb <file>]
 *             [--serial file:PATH] [--entry ADDR] [--max-insns N] [--smp N]
 *   oemu --help
 *
 * `run` boots a static AArch64 ET_EXEC image: read the file, hand it to
 * oemu_elf_load, give the guest a stack (the loader deliberately does not
 * synthesise one), run it, and exit with the status code the guest asked for.
 * The guest's own stdout passes straight through, so
 * `oemu run prog.elf | diff - expected.txt` works.
 *
 * `boot` (M2c, M4a) starts an AArch64 Linux Image at EL1 with identity
 * mapping: Image-loader placement, a device-tree blob in x0, a real PL011,
 * and a PSCI conduit that powers the machine down on SYSTEM_OFF. The machine layout mirrors the
 * QEMU virt oracle baseline (docs/linux-minimal-qemu.md) so the same guest runs on both: RAM
 * 256 MiB at 0x40000000, the image at 0x40080000 (the AArch64 Image load
 * address; the header's `b _start` makes the load address a valid entry point,
 * so --entry defaults to it), and a stopgap UART at 0x09000000 (QEMU virt's
 * UART0 -- a temporary device only until M4a's real PL011 replaces it; the
 * write path emits one byte to stdout, every register reads 0). x0..x3 come in
 * x0 carries the DTB, which the embedded fixture (tests/fixtures/boot.dtb,
 * dtc-compiled from the mirrored .dts) supplies unless -dtb overrides it.
 *
 * The exit protocol, until PSCI exists (M4b): the guest writes the sentinel
 * byte 0x04 (EOT) to the UART data register. The device callback raises the
 * machine's sticky POWERDOWN event -- which is exactly what the event was
 * designed to carry -- and the run loop's env->halted sees it at the next
 * instruction boundary, so oemu exits with the machine's requested code. A
 * guest that merely parks in WFI instead is reported as EXIT_BLOCKED: with one
 * vCPU and no interrupt source, parked means deadlocked.
 *
 * Diagnostics go to stderr and the guest's output to stdout, kept strictly apart
 * so the pass-through stream stays byte-clean. Only the fixed read chunk lives on
 * the stack; the image itself is held by oemu_buffer, whose allocation goes
 * through the library's allocator seam like everything else in src/.
 */
#include "oemu/aspace.h"
#include "oemu/buffer.h"
#include "oemu/elf.h"
#include "oemu/exec.h"
#include "oemu/fdt.h"
#include "oemu/gtimer.h"
#include "oemu/image.h"
#include "oemu/machine.h"
#include "oemu/memory.h"
#include "oemu/psci.h"
#include "oemu/status.h"
#include "oemu/sysenv.h"
#include "oemu/sysreg.h"
#include "oemu/vcpu.h"
#include "oemu/virt_dtb.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "boot_dtb.h" /* generated: the fixture blob as bytes */
#include "oemu/gicv2.h"
#include "oemu/pl011.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/random.h>
#include <termios.h>
#include <unistd.h>

/*
 * Stack policy. A freestanding guest's crt0 only needs a 16-byte-aligned SP --
 * there is no argv or auxv to lay down -- so a plain fixed region suffices. Place
 * it well above the loaded image; if the image is already mapped high, put the
 * stack one region-size past it instead, so the two can never collide. Both the
 * base and the size are powers of two, so the resulting SP is aligned for free.
 */
#define STACK_BASE  ((uint64_t)0x10000000ULL) /* 256 MiB: above any static image */
#define STACK_SIZE  ((uint64_t)0x00100000ULL) /* 1 MiB */
#define STACK_ALIGN 16U

/* The guest needs ~3 regions (a couple of PT_LOAD plus the stack); 32 leaves room
 * for a many-segment image before the loader reports the table is full. */
#define REGION_CAPACITY 32U

/*
 * Exit statuses the CLI returns for its own reasons. A guest exit code is passed
 * through masked to 8 bits exactly as a shell would see it, so these sentinels
 * only apply when the guest never got to call exit. 1 is the ordinary failure
 * code; a guest could legitimately exit(1), and that is fine -- the distinction
 * is in stderr, which stays silent on a clean guest exit.
 */
#define EXIT_ERROR   1
#define EXIT_USAGE   2
#define EXIT_TIMEOUT 3
/* The guest sat in WFI/WFE with nothing that could wake it: with one vCPU and
 * no interrupt source that is a deadlock, and a deadlock deserves its own code
 * so a test can tell "budget too small" (3) from "guest cannot make progress"
 * (4). */
#define EXIT_BLOCKED 4

/*
 * `oemu boot` machine layout, chosen to match the QEMU virt oracle baseline so
 * one guest binary runs on both (docs/verification-strategy.md L4): RAM base
 * and size from docs/linux-minimal-qemu.md, image address from the booting.rst
 * protocol, UART address from virt's memory map.
 */
#define BOOT_RAM_BASE      ((uint64_t)0x40000000ULL)
#define BOOT_RAM_DEFAULT   ((uint64_t)1024U) /* MiB; the fixture DTB's memory node */
#define BOOT_DTB_SLOT      1U                /* one more region: the DTB rides the bus too */
#define BOOT_IMAGE_BASE    ((uint64_t)0x40080000ULL) /* booting.rst kernel load address */
#define BOOT_UART_BASE     ((uint64_t)0x09000000ULL) /* virt UART0 -- oracle-portable */
#define BOOT_UART_SIZE     ((uint64_t)0x00001000ULL)
#define BOOT_GIC_DIST_BASE ((uint64_t)0x08000000ULL) /* virt GICD -- DT reg[0] */
#define BOOT_GIC_DIST_SIZE ((uint64_t)0x00010000ULL)
#define BOOT_GIC_CPU_BASE  ((uint64_t)0x08010000ULL) /* virt GICC -- DT reg[1] */
#define BOOT_GIC_CPU_SIZE  ((uint64_t)0x00010000ULL)
#define BOOT_GIC_LINES     64U /* two groups: NR_IRQS 64, as the oracle */
/* The PL011's single line: /interrupts = <0 1 4> -> SPI, offset 1 -> id 33. */
#define BOOT_UART_SPI 33U
/* The generic timer's PPIs, read off the DT's /timer interrupts in binding
 * order <1,13> <1,14> <1,11> <1,10>, which the arm,armv8-timer binding reads as
 * [secure phys, non-secure phys, virtual, hyp] = [29, 30, 27, 26]. Those ids are
 * the architecture's, not a choice: PPI 26 is the EL2 physical timer, 27 the
 * non-secure EL1 *virtual* one, 30 the non-secure physical, 29 the secure. Our
 * guest therefore clocks itself on the virtual comparator -- it writes
 * CNTV_CTL/CNTV_CVAL and unmasks INTID 27, both measured off QEMU-booted guest
 * behaviour -- and the earlier wiring here (virtual comparator on 26, the hyp
 * timer's line) meant its comparator expired against a line the guest never
 * enabled: no tick, jiffies frozen, the async device probe never ran,
 * /dev/console never opened, and pid 1 spun in a write() retry loop forever.
 * So: virtual comparator -> 27, non-secure physical comparator -> 30. The 29
 * (secure) and 26 (EL2 physical) lines stay undriven because oemu models
 * neither a secure world nor an EL2 timer, and the distributor keeps a line the
 * guest never enabled quiet anyway. */
#define BOOT_TIMER_PHYS_PPI 30U
#define BOOT_TIMER_VIRT_PPI 27U
#define BOOT_DTB_MAX        ((size_t)65536U) /* a DTB over 64 KiB is a mistake */
#define BOOT_CMDLINE_MAX    (256U)           /* writable boot line width */
#define BOOT_BOOTLINE_LEN   (257U)           /* the fixture property: pad + NUL */
/* The initrd sits at the three-quarter mark of RAM -- clear of kernel text at
 * the base, clear of the DTB at the half mark, and clear of the boot stack at
 * the very top by BOOT_INITRD_MARGIN. A tree that advertises the initrd must
 * not let it overlap what the kernel unpacks there. */
#define BOOT_INITRD_MARGIN   ((uint64_t)16U << 20) /* headroom to the RAM top */
#define BOOT_REGION_CAPACITY 8U                    /* RAM + UART + DTB + room for M4 devices */
/* Instructions per scheduler slice. One vCPU, so a quantum is purely the
 * latency bound between machine-event polls; 1M keeps a stuck guest inside
 * the 60 s test budget while keeping syscall-free slices cheap. */
#define BOOT_QUANTUM UINT64_C(1000000)
/* How often the level-driven interrupt inputs are re-sampled. The pins are the
 * GIC's word, and the GIC only knows what main.c last pushed into it, so the
 * sampling period is the staleness of every level line: a comparator the guest
 * has re-armed into the future keeps its PPI pending until the next sample, and
 * the guest re-enters the handler every time it unmasks. At the boot slice of
 * 1e6 that nesting ran the stack down into the page tables. */
#define BOOT_LEVEL_QUANTUM UINT64_C(64)

/* Round `v` up to a multiple of the power-of-two `align`. */
static uint64_t align_up(uint64_t v, uint64_t align) {
  return (v + (align - 1U)) & ~(align - 1U);
}

/* Pick a stack base that cannot overlap the just-loaded segments. */
static uint64_t stack_base_for(const oemu_elf_image *img) {
  const uint64_t base = STACK_BASE;
  const uint64_t just_past_image = align_up(img->load_max, STACK_SIZE) + STACK_SIZE;
  return (just_past_image > base) ? just_past_image : base;
}

static void print_usage(FILE *out) {
  (void)fputs("usage: oemu run <image.elf> [--max-insns N]\n", out);
  (void)fputs(
      "       oemu boot -kernel <Image> [-initrd <cpio>] [-append <cmdline>] "
      "[-m MiB] [-dtb <file>]\n",
      out);
  (void)fputs("                 [--serial file:PATH|stdio] [--entry ADDR] [--max-insns N]\n",
              out);
  (void)fputs(
      "       --serial stdio wires the console to our stdin: a guest parked at a\n"
      "       prompt is waiting for a keystroke, and EOF on stdin ends the run.\n",
      out);
  (void)fputs("       oemu --help\n", out);
}

/*
 * Slurps the whole file into `buf`. A fixed stack chunk means no allocation here
 * except the buffer's own (through the seam). Returns 0 on success; on any
 * failure the reason is already on stderr.
 */
static int read_file(const char *path, oemu_buffer *buf) {
  FILE *f = fopen(path, "rb");
  if (f == NULL) {
    (void)fprintf(stderr, "oemu: cannot open '%s': %s\n", path, strerror(errno));
    return -1;
  }
  unsigned char chunk[4096];
  size_t n = 0U;
  oemu_status st = OEMU_OK;
  while ((n = fread(chunk, 1U, sizeof(chunk), f)) > 0U) {
    st = oemu_buffer_append(buf, chunk, n);
    if (st != OEMU_OK) {
      break; /* out of memory; reported below from the status */
    }
  }
  int rd_err = ferror(f);
  if (fclose(f) != 0 && rd_err == 0) {
    rd_err = errno;
  }
  if (st != OEMU_OK) {
    (void)fprintf(stderr, "oemu: reading '%s': %s\n", path, oemu_status_str(st));
    return -1;
  }
  if (rd_err != 0) {
    (void)fprintf(stderr, "oemu: reading '%s': %s\n", path, strerror(rd_err));
    return -1;
  }
  return 0;
}

/* Boots `path` and returns the process exit status. */
static int run(const char *path, uint64_t max_insns) {
  oemu_buffer image;
  oemu_memory mem = {0};
  oemu_cpu cpu = {0};
  oemu_sysenv env = {0};
  oemu_elf_image img = {0};
  oemu_status st = OEMU_OK;
  uint64_t stack_base = 0U;
  uint64_t sp = 0U;
  uint64_t completed = 0U;
  int result = EXIT_ERROR;

  if (oemu_buffer_init(&image, 0U) != OEMU_OK) {
    (void)fputs("oemu: out of memory\n", stderr);
    return EXIT_ERROR;
  }
  if (read_file(path, &image) != 0) {
    goto done;
  }

  st = oemu_memory_init(&mem, REGION_CAPACITY);
  if (st != OEMU_OK) {
    (void)fprintf(stderr, "oemu: memory init failed: %s\n", oemu_status_str(st));
    goto done;
  }

  st = oemu_elf_load(&mem, oemu_buffer_data(&image), (uint64_t)oemu_buffer_len(&image), &img);
  if (st != OEMU_OK) {
    (void)fprintf(stderr, "oemu: %s: %s\n", path, oemu_status_str(st));
    goto done;
  }

  stack_base = stack_base_for(&img);
  st = oemu_memory_map(&mem, stack_base, STACK_SIZE, OEMU_PERM_READ | OEMU_PERM_WRITE);
  if (st != OEMU_OK) {
    (void)fprintf(stderr, "oemu: mapping the stack failed: %s\n", oemu_status_str(st));
    goto done;
  }
  sp = (stack_base + STACK_SIZE) & ~((uint64_t)(STACK_ALIGN - 1U));

  oemu_sysenv_init(&env, stdout); /* guest fd 1/2 -> our stdout */
  st = oemu_cpu_init(&cpu, img.entry, sp);
  if (st != OEMU_OK) {
    (void)fprintf(stderr, "oemu: cpu init failed: %s\n", oemu_status_str(st));
    goto done;
  }

  st = oemu_exec_run(&cpu, &mem, &env, max_insns, &completed);
  if (oemu_sysenv_exited(&env)) {
    result = oemu_sysenv_exit_code(&env) & 0xFF; /* a shell-visible exit code */
  } else if (st == OEMU_ERR_TIMEOUT) {
    (void)fprintf(stderr, "oemu: timeout after %" PRIu64 " instructions\n", completed);
    result = EXIT_TIMEOUT;
  } else {
    (void)fprintf(stderr, "oemu: run failed: %s\n", oemu_status_str(st));
    result = EXIT_ERROR;
  }

done:
  oemu_memory_dispose(&mem);
  oemu_buffer_dispose(&image);
  return result;
}

/* --- `oemu boot` (M2c) ----------------------------------------------------- */

/*
 * The boot console is a real PL011 (src/dev/pl011.c): the fixture DTB's
 * /pl011@9000000 node promises one, and guests hold it to that promise --
 * they read the PID registers, program CR, and expect a FIFO. The sink
 * drains TX bytes into the serial file (or stdout) on every scheduler
 * slice, so markers appear as the guest writes them.
 */
typedef struct boot_serial {
  FILE *out;
} boot_serial;

static void boot_serial_sink(void *user, unsigned char byte) {
  boot_serial *ser = (boot_serial *)user;
  (void)fputc((int)byte, ser->out);
  (void)fflush(ser->out); /* markers must be visible even if the run dies later */
}

/* The run loop's `halted`: either a machine event (a device asked) or the
 * PSCI power state -- SYSTEM_OFF lands on both sides at once, but checking
 * both keeps each seam honest on its own. `syscall` stays NULL: at EL1 an
 * SVC is an exception into the guest's vectors, not a host call. */
typedef struct boot_env {
  oemu_machine *machine;
  oemu_psci *psci;
} boot_env;

static bool boot_halted(const void *ctx) {
  const boot_env *benv = (const boot_env *)ctx;
  return (oemu_machine_event_peek(benv->machine) != OEMU_MACHINE_EVENT_NONE) ||
         benv->psci->halted;
}

/* HVC/SMC arrive here before any exception: PSCI answers what it knows and
 * the instruction steps; anything else falls through as the undefined
 * exception the architecture prescribes for a bare-metal monitor call.
 * A SYSTEM_OFF that landed powers the machine down, so the run loop's
 * event poll turns it into a clean exit 0. */
static bool boot_fw_call(void *ctx, bool is_hvc, uint16_t imm, const uint64_t args[3],
                         uint64_t *ret0) {
  (void)is_hvc; /* the fixture negotiates the conduit; both answer alike */
  (void)imm;    /* oemu implements the 0.1-0.2 function IDs; imm is unused */
  boot_env *benv = (boot_env *)ctx;
  uint64_t r0 = 0U;
  if (!oemu_psci_dispatch(benv->psci, args[0], &r0)) {
    return false;
  }
  *ret0 = r0;
  if (benv->psci->halted) {
    oemu_machine_poweroff(benv->machine, 0);
  }
  if (benv->psci->reset) {
    oemu_machine_reset(benv->machine);
  }
  return true;
}

static uint32_t be32(const unsigned char *p) {
  return ((uint32_t)p[0] << 24U) | ((uint32_t)p[1] << 16U) | ((uint32_t)p[2] << 8U) | p[3];
}

static size_t strnlen(const char *s, size_t cap) {
  size_t n = 0U;
  while ((n < cap) && (s[n] != '\0')) {
    n++;
  }
  return n;
}

/* The four FDT wire tokens the patchers dispatch on (spec values; the fdt
 * module owns the same four in its internal header). */
#define BOOT_FDT_MAGIC            0xD00DFEEDU
#define BOOT_FDT_TOKEN_BEGIN_NODE 0x00000001U
#define BOOT_FDT_TOKEN_END_NODE   0x00000002U
#define BOOT_FDT_TOKEN_PROP       0x00000003U
#define BOOT_FDT_TOKEN_END        0x00000009U

/*
 * Patch the chosen /chosen/bootline property in place inside a finished
 * blob: the fixture carries a 256-space pad exactly so this write never
 * changes the blob's size, offsets, or dtc-identity. Returns false when
 * the blob has no bootline to write into.
 */
static bool dtb_patch_bootline(unsigned char *blob, size_t blob_len, const char *cmdline) {
  const size_t clen = strlen(cmdline);
  if (clen >= BOOT_CMDLINE_MAX) {
    (void)fprintf(stderr, "oemu: -append: command line of %zu bytes exceeds %u\n", clen,
                  BOOT_CMDLINE_MAX);
    return false;
  }
  const uint32_t off_struct = be32(blob + 8U);
  const uint32_t off_strings = be32(blob + 12U);
  unsigned char *p = blob + off_struct;
  unsigned char *const end = blob + blob_len;
  int depth = 0; /* only /chosen's bootline is ours; deeper matches are strangers */
  while (p + 4U <= end) {
    const uint32_t token = be32(p);
    p += 4U;
    if (token == BOOT_FDT_TOKEN_END) {
      break;
    }
    if (token == BOOT_FDT_TOKEN_END_NODE) {
      depth--;
      continue;
    }
    if (token == BOOT_FDT_TOKEN_BEGIN_NODE) {
      const size_t nl = strnlen((const char *)p, (size_t)(end - p));
      const bool is_chosen = ((depth == 1) && (nl == 6U) && (memcmp(p, "chosen", 6U) == 0));
      depth++;
      p += ((nl + 1U + 3U) / 4U) * 4U;
      if (!is_chosen) {
        continue;
      }
      while (p + 4U <= end) {
        const uint32_t t = be32(p);
        p += 4U;
        if (t == BOOT_FDT_TOKEN_END_NODE) {
          depth--;
          break;
        }
        if (t != BOOT_FDT_TOKEN_PROP) {
          return false; /* chosen's children are not our problem */
        }
        const uint32_t dlen = be32(p);
        const uint32_t name_off = be32(p + 4U);
        const char *nm = (const char *)blob + off_strings + name_off;
        if ((dlen == BOOT_BOOTLINE_LEN) && (strcmp(nm, "bootline") == 0)) {
          (void)memset(p + 8U, ' ', BOOT_CMDLINE_MAX);
          (void)memcpy(p + 8U, cmdline, clen);
          p[8U + clen] = (unsigned char)'\0';
          p[BOOT_BOOTLINE_LEN + 7U] = (unsigned char)'\0'; /* the pad's own terminator */
          return true;
        }
        p += 8U + (((dlen + 3U) / 4U) * 4U);
      }
      return false;
    }
    if (token == BOOT_FDT_TOKEN_PROP) {
      const uint32_t dlen = be32(p);
      p += 8U + (((dlen + 3U) / 4U) * 4U);
      continue;
    }
    return false;
  }
  return false;
}

/*
 * Rewrite the single /memory@ node's reg <base size> pair to describe the
 * RAM window we build. The fixture has exactly one memory node, 1 GiB at
 * 0x40000000; `oemu boot -m` rewrites the size cell here so the tree never
 * claims more (or different) memory than the machine has.
 */
static bool dtb_patch_node(unsigned char *blob, size_t blob_len, uint64_t base, uint64_t size) {
  const uint32_t off_struct = be32(blob + 8U);
  const uint32_t off_strings = be32(blob + 12U);
  unsigned char *p = blob + off_struct;
  unsigned char *const end = blob + blob_len;
  int depth = 0;
  bool in_wanted = false;
  const char *want = "memory@";
  while (p + 4U <= end) {
    const uint32_t token = be32(p);
    p += 4U;
    if (token == BOOT_FDT_TOKEN_END) {
      break;
    }
    if (token == BOOT_FDT_TOKEN_END_NODE) {
      depth--;
      in_wanted = false;
      continue;
    }
    if (token == BOOT_FDT_TOKEN_BEGIN_NODE) {
      const size_t nl = strnlen((const char *)p, (size_t)(end - p));
      in_wanted = ((depth == 1) && (nl >= 7U) && (memcmp(p, want, 7U) == 0));
      depth++;
      p += ((nl + 1U + 3U) / 4U) * 4U;
      continue;
    }
    if (token == BOOT_FDT_TOKEN_PROP) {
      const uint32_t dlen = be32(p);
      const uint32_t name_off = be32(p + 4U);
      const char *nm = (const char *)blob + off_strings + name_off;
      if (in_wanted && (strcmp(nm, "reg") == 0) && (dlen >= 16U)) {
        unsigned char *v = p + 8U;
        for (unsigned b = 0U; b < 8U; b++) {
          v[b] = (unsigned char)(base >> (56U - 8U * b));
          v[8U + b] = (unsigned char)(size >> (56U - 8U * b));
        }
        return true;
      }
      p += 8U + (((dlen + 3U) / 4U) * 4U);
      continue;
    }
    return false;
  }
  return false;
}

/* Read a DTB: the caller's file, or the embedded fixture compiled from
 * tests/fixtures/boot.dts. `bytes`/`len` are filled for the caller to free. */
static int dtb_load(const char *path, unsigned char **bytes, size_t *len) {
  if (path != NULL) {
    oemu_buffer file;
    if (oemu_buffer_init(&file, 0U) != OEMU_OK) {
      return -1;
    }
    if (read_file(path, &file) != 0) {
      oemu_buffer_dispose(&file);
      return -1;
    }
    *len = oemu_buffer_len(&file);
    *bytes = (unsigned char *)oemu_allocator_get()->alloc(*len, NULL);
    if (*bytes == NULL) {
      oemu_buffer_dispose(&file);
      return -1;
    }
    (void)memcpy(*bytes, oemu_buffer_data(&file), *len);
    oemu_buffer_dispose(&file);
    return 0;
  }
  const size_t n = sizeof(oemu_boot_dtb);
  *bytes = (unsigned char *)oemu_allocator_get()->alloc(n, NULL);
  if (*bytes == NULL) {
    return -1;
  }
  (void)memcpy(*bytes, oemu_boot_dtb, n);
  *len = n;
  return 0;
}

/*
 * The cooperative run loop: one quantum at a time, then a look at the machine.
 * Every exit reason is decided here -- the guest asks through a device, the CLI
 * acts. A separate function so `boot` itself keeps the goto-clean shape the
 * project's -Wjump-misses-init demands of functions with a cleanup label.
 */
/* Where the guest was when we gave up. A boot emulator that cannot say
 * "the guest is stuck at PC 0x..., having taken an exception with this
 * syndrome" is only half a debugger, and a hang is the single most common
 * early-boot failure -- so the timeout path reports the architectural
 * context (PC, EL, and the EL1 exception latch) rather than just a count. */
static void boot_sysrd(const oemu_vcpu *vcpu, uint32_t sel, uint64_t *out) {
  const oemu_status st = oemu_sysreg_read(&vcpu->sysregs, sel, out);
  (void)st;
}

static void boot_hang_report(const oemu_vcpu *vcpu, const char *why, uint64_t insns) {
  uint64_t el = 0U;
  uint64_t elr = 0U;
  uint64_t esr = 0U;
  uint64_t far = 0U;
  uint64_t spsr = 0U;
  uint64_t sctlr = 0U;
  uint64_t midr = 0U;
  boot_sysrd(vcpu, OEMU_SYSREG_CURRENT_EL, &el);
  boot_sysrd(vcpu, OEMU_SYSREG_ELR_EL1, &elr);
  boot_sysrd(vcpu, OEMU_SYSREG_ESR_EL1, &esr);
  boot_sysrd(vcpu, OEMU_SYSREG_FAR_EL1, &far);
  boot_sysrd(vcpu, OEMU_SYSREG_SPSR_EL1, &spsr);
  boot_sysrd(vcpu, OEMU_SYSREG_SCTLR_EL1, &sctlr);
  boot_sysrd(vcpu, OEMU_SYSREG_MIDR_EL1, &midr);
  (void)fprintf(stderr, "oemu: %s after %" PRIu64 " instructions\n", why, insns);
  (void)fprintf(stderr,
                "oemu:   PC=0x%016" PRIx64 " EL=%" PRIu64 " SCTLR_EL1=0x%" PRIx64
                " MIDR_EL1=0x%" PRIx64 "\n",
                vcpu->cpu.regs.pc, el, sctlr, midr);
  (void)fprintf(stderr,
                "oemu:   ESR_EL1=0x%" PRIx64 " FAR_EL1=0x%016" PRIx64 " ELR_EL1=0x%016" PRIx64
                " SPSR_EL1=0x%" PRIx64 "\n",
                esr, far, elr, spsr);
}

/* Where a stalled console left the UART: enough state to tell "the guest never
 * printed it" from "the driver never drained it". A boot that ends with bytes
 * queued, or with a latched-but-unhandled interrupt, or with the modem flags
 * flow-controlling transmit, is a device-model bug and says so here. */
static void boot_uart_report(const oemu_pl011 *uart) {
  if (uart == NULL) {
    return;
  }
  (void)fprintf(stderr,
                "oemu:   uart: tx_queued=%u tx_emitted=%" PRIu64 " tx_dropped=%" PRIu64
                " rx_queued=%u CR=0x%03x FR=0x%02x RIS=0x%03x IMSC=0x%03x\n",
                uart->tx_count, uart->tx_emitted, uart->tx_dropped, uart->rx_count, uart->cr,
                uart->fr, uart->ris, uart->imsc);
}

/* One line to stderr if the console dropped anything: a boot gate decides on
 * the log it captured, and a truncated log must never be read as "the guest
 * never printed it". */
static void boot_console_warn(const oemu_pl011 *uart, uint64_t input_dropped) {
  if (input_dropped != 0ULL) {
    (void)fprintf(
        stderr, "oemu: console dropped %" PRIu64 " input byte(s): the guest never read them\n",
        input_dropped);
  }
  const uint64_t lost = oemu_pl011_tx_dropped(uart);
  if (lost != 0U) {
    (void)fprintf(stderr, "oemu: console dropped %" PRIu64 " TX bytes (log is truncated)\n",
                  lost);
  }
}

/* Sample every level-driven interrupt input and hand the GIC's verdict to the
 * vCPU. Called between sub-slices, because the pending bits are only as fresh
 * as the last call: a PPI whose comparator the guest has already re-armed must
 * stop being pending before the guest unmasks again, or it re-enters the
 * handler and walks the stack down. */
static void boot_refresh_levels(oemu_vcpu *vcpu, oemu_gicv2 *gic, oemu_pl011 *uart) {
  /* The PL011 is a level source on GIC SPI 33 (the DT's /interrupts). Refresh
   * the distributor's pending bit from the UART's live level each slice, so a
   * received byte reaches the driver as interrupt 33 -- not a flat pin whose
   * GICC_IAR the driver would read back as spurious. The vCPU's IRQ is then
   * the GIC's word alone. */
  oemu_gicv2_set_pending(gic, BOOT_UART_SPI, oemu_pl011_irq_level(uart) != 0);
  /* The generic timer's clockevent comparator is level-high once the counter
   * passes it, so refresh the DT-declared PPIs from the live comparator each
   * slice. Without this the counter moves but jiffies never tick and an idle
   * guest soft-locks waiting for a timer IRQ that never arrives. */
  const oemu_sysregs *sr = &vcpu->sysregs;
  /* /timer's interrupts are <1,13>,<1,14>,<1,11>,<1,10> = PPIs 29, 30, 27, 26,
   * and the generic arch timer picks its event PPI from that order: a guest
   * that came up at EL2 (which is what our firmware hands over) programs the
   * *virtual* comparator and takes PPI 26, while a guest that believes it owns
   * the physical timer programs CNTP_* and takes PPI 30. Both banks therefore
   * drive their own PPI -- conflating them is what kept this guest tickless:
   * the virtual comparator was wired to 30, whose handler is the physical
   * timer's, so the counter ran, no handler ever ran, jiffies froze at 2, no
   * async probe ran, /dev/console never opened, and pid 1 spun in a write()
   * retry loop forever. */
  oemu_gicv2_set_pending(gic, BOOT_TIMER_VIRT_PPI,
                         oemu_gtimer_pending(sr->cntvct - sr->cntvoff_el1, sr->cntv_ctl_el1,
                                             sr->cntv_cval_el1) != 0);
  oemu_gicv2_set_pending(
      gic, BOOT_TIMER_PHYS_PPI,
      oemu_gtimer_pending(sr->cntvct, sr->cntp_ctl_el1, sr->cntp_cval_el1) != 0);
  oemu_vcpu_set_irq(vcpu, oemu_gicv2_irq_level(gic) != 0);
}

/* Hand one slice of typed input to the UART, holding back what it cannot take.
 *
 * The device refuses a byte while UARTEN|RXE is clear -- correct for a receiver
 * that is not listening -- but the driver clears RXE as a matter of course
 * (`pl011_start_tx` disables the receiver before each transmit, half-duplex
 * style, and `pl011_stop_tx` restores it), and the oracle is forgiving about the
 * resulting window: measured at an idle shell prompt its CR is 0x0f01, i.e. RXE
 * clear, yet a line typed there is still delivered and echoed. So a refused byte
 * must be held and retried, not dropped: the byte has already left the host
 * terminal, and the old drop-on-refuse meant a keystroke that arrived during a
 * transmission -- or before /init opened the tty -- simply vanished, which is
 * exactly how the SHELL_ALIVE marker went missing. The queue is bounded and the
 * overflow is counted rather than silently lost. */
#define BOOT_HELD_MAX 256U

/* Deliver what we are holding and keep back what the device refused: the driver
 * clears RXE every time it transmits (pl011_start_tx, half-duplex), so a
 * keystroke can be legitimately unwelcome at the moment it arrives. */
static void boot_inject_held(oemu_pl011 *uart, unsigned char *held, size_t *held_len) {
  size_t kept = 0U;
  for (size_t i = 0U; i < *held_len; ++i) {
    if (oemu_pl011_inject(uart, held[i]) == OEMU_OK) {
      continue;
    }
    held[kept++] = held[i];
  }
  *held_len = kept;
}

static void boot_pump_stdin(oemu_pl011 *uart, unsigned char *held, size_t *held_len,
                            uint64_t *dropped, bool *eof) {
  unsigned char c = 0U;
  for (;;) {
    /* poll(2), not O_NONBLOCK: the blocking wait below borrows the descriptor
     * and has to be able to hand it back in any state, and a pump that blocks
     * in read() never returns to run the guest it just fed -- which is how a
     * held-over line once sat in the UART ring while the run loop hung. */
    struct pollfd waiting = {STDIN_FILENO, (short)POLLIN, 0};
    if (poll(&waiting, 1U, 0) <= 0) {
      break; /* nothing typed at this instant (or the console is gone) */
    }
    const ssize_t got = read(STDIN_FILENO, &c, 1U);
    if (got < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (got == 0) {
      *eof = true;
      break; /* the pipe is closed: no keystroke is ever coming */
    }
    if (*held_len >= BOOT_HELD_MAX) {
      (*dropped)++; /* a guest that never opens its console loses input */
    } else {
      held[(*held_len)++] = c;
    }
  }
  boot_inject_held(uart, held, held_len);
}

/* A parked vCPU whose console is wired to our stdin is an *idle* guest, not a
 * dead one: the shell is sitting in read(2) waiting for someone to type, which is
 * exactly what the boot gate does -- its interactive line arrives seconds after
 * the prompt. QEMU's -serial stdio simply blocks in read() here; declaring the
 * boot blocked instead loses the marker no matter how far the guest got. So wait
 * for the next byte (with the O_NONBLOCK we set, cleared), feed it, and only give
 * up once stdin has reported EOF. */
static bool boot_await_input(oemu_pl011 *uart, unsigned char *held, size_t *held_len,
                             uint64_t *dropped, bool *eof) {
  if (*held_len != 0U) {
    boot_inject_held(uart, held, held_len);
    if (*held_len != 0U) {
      return true; /* input is still queued: the guest has a reason to run */
    }
  }
  if (*eof) {
    return false;
  }
  const int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
  if ((flags >= 0) && ((flags & O_NONBLOCK) != 0)) {
    (void)fcntl(STDIN_FILENO, F_SETFL, flags & ~O_NONBLOCK);
  }
  unsigned char c = 0U;
  bool got = false;
  ssize_t n = read(STDIN_FILENO, &c, 1U);
  while ((n < 0) && (errno == EINTR)) {
    n = read(STDIN_FILENO, &c, 1U);
  }
  if (flags >= 0) {
    (void)fcntl(STDIN_FILENO, F_SETFL, flags); /* exactly the flags we found */
  }
  if (n == 1) {
    got = true;
    if (*held_len < BOOT_HELD_MAX) {
      held[(*held_len)++] = c;
    } else {
      (*dropped)++;
    }
  } else if (n == 0) {
    *eof = true;
    return false;
  } else {
    *eof = true; /* a console we cannot read will not become readable */
    return false;
  }
  (void)boot_pump_stdin(uart, held, held_len, dropped, eof); /* the rest of the line */
  return got;
}

static int boot_run(oemu_vcpu *vcpu, oemu_machine *machine, oemu_pl011 *uart, oemu_gicv2 *gic,
                    bool pump_stdin, uint64_t max_insns) {
  uint64_t budget = max_insns;
  unsigned char held[BOOT_HELD_MAX];
  size_t held_len = 0U;
  uint64_t held_drop = 0U;
  bool stdin_eof = false;
  bool idle_announced = false;
  for (;;) {
    const uint64_t slice = (budget < BOOT_QUANTUM) ? budget : BOOT_QUANTUM;
    uint64_t done = 0U;
    oemu_status st = OEMU_OK;
    while ((done < slice) && ((st == OEMU_OK) || (st == OEMU_ERR_TIMEOUT))) {
      const uint64_t remaining = slice - done;
      const uint64_t chunk = (remaining < BOOT_LEVEL_QUANTUM) ? remaining : BOOT_LEVEL_QUANTUM;
      uint64_t stepped = 0U;
      st = oemu_vcpu_run(vcpu, chunk, &stepped);
      done += stepped;
      boot_refresh_levels(vcpu, gic, uart);
      if (stepped == 0U) {
        break; /* halted: the refresh above decides whether the next slice wakes it */
      }
    }
    budget -= done;
    (void)oemu_pl011_pump(uart); /* the console drains on every slice boundary */
    /* Interactive console: drain whatever the host typed into the UART RX ring.
     * A byte the ring cannot hold is dropped, exactly as QEMU drops an early
     * byte before the driver enables the receiver. */
    if (pump_stdin) {
      boot_pump_stdin(uart, held, &held_len, &held_drop, &stdin_eof);
    }
    const oemu_machine_event ev = oemu_machine_event_peek(machine);
    if (ev == OEMU_MACHINE_EVENT_POWERDOWN) {
      boot_console_warn(uart, held_drop);
      return machine->exit_code & 0xFF; /* the code travels as a shell sees it */
    }
    if (ev == OEMU_MACHINE_EVENT_RESET) {
      (void)fputs("oemu: guest requested a reset, but restart is not implemented\n", stderr);
      return EXIT_ERROR;
    }
    if (st == OEMU_ERR_BLOCKED) {
      if (pump_stdin && boot_await_input(uart, held, &held_len, &held_drop, &stdin_eof)) {
        if (!idle_announced) {
          idle_announced = true;
          (void)fputs("oemu: guest idle at the console; handing over input\n", stderr);
          boot_uart_report(uart); /* did the byte reach the device, or is it still ours? */
          /* The two ends of the interrupt wire, side by side: a level at the
           * device that the distributor does not pass, or one it passes that the
           * core does not take, are different bugs and this line tells them
           * apart without a rebuild. */
          (void)fprintf(stderr, "oemu:   irq: device=%d gic=%d into_core=%d\n",
                        oemu_pl011_irq_level(uart), oemu_gicv2_irq_level(gic),
                        vcpu->irq_level ? 1 : 0);
          /* The distributor's whole reason for holding the line back, on one
           * line: a source that is disabled, masked by the priority mask,
           * already active, targeted elsewhere, or level-configured but latched
           * as edge are five different bugs, and each is one field here. */
          (void)fprintf(stderr,
                        "oemu:   gic: ctl=0x%x cpu_ctl=0x%x pmr=0x%x running=%u | irq %u"
                        " en=%u pend=%u act=%u cfg=%u grp=%u pri=0x%02x tgt=0x%02x\n",
                        gic->ctl, gic->cpu_ctl, gic->cpu_pmr, gic->running_pri, BOOT_UART_SPI,
                        gic->enable[BOOT_UART_SPI], gic->pending[BOOT_UART_SPI],
                        gic->active[BOOT_UART_SPI], gic->config[BOOT_UART_SPI],
                        gic->group[BOOT_UART_SPI], gic->priority[BOOT_UART_SPI],
                        gic->target[BOOT_UART_SPI]);
        }
        boot_refresh_levels(vcpu, gic, uart); /* the byte we just fed may now wake it */
        oemu_vcpu_rearm(vcpu);
        continue;
      }
      boot_hang_report(vcpu, "guest parked", max_insns - budget);
      boot_uart_report(uart);
      (void)fputs("oemu: guest parked at WFI/WFE with nothing to wake it\n", stderr);
      return EXIT_BLOCKED;
    }
    if (st != OEMU_OK && st != OEMU_ERR_TIMEOUT) {
      (void)fprintf(stderr, "oemu: boot failed: %s\n", oemu_status_str(st));
      boot_hang_report(vcpu, "boot stopped", max_insns - budget);
      return EXIT_ERROR;
    }
    if (budget == 0U) {
      boot_hang_report(vcpu, "timeout", max_insns);
      boot_uart_report(uart);
      boot_console_warn(uart, held_drop);
      return EXIT_TIMEOUT;
    }
    oemu_vcpu_rearm(vcpu);
  }
}

/* Everything `oemu boot` was told, after parsing and defaulting. */
typedef struct boot_opts {
  const char *kernel;      /* -kernel: required */
  const char *cmdline;     /* -append: NULL when absent */
  const char *dtb;         /* -dtb:    NULL -> the generated virt tree */
  const char *initrd;      /* -initrd: NULL -> no initial ramdisk */
  const char *serial_path; /* --serial file:PATH; NULL -> stdout */
  uint64_t ram_mib;        /* -m:      MiB, defaulting to the fixture's 1 GiB */
  uint64_t entry;          /* --entry: 0 -> the Image header's own entry */
  uint64_t max_insns;      /* --max-insns */
  bool stdio;              /* -serial stdio: console is interactive (RX wired) */
} boot_opts;

/* Bytes for /chosen/rng-seed, which the guest's early_init_dt() feeds to
 * add_bootloader_randomness() before nopping the property out of the live tree.
 * The oracle always injects a seed, so without one our guest reaches the
 * initcalls with an uninitialised CRNG -- a divergence no amount of CPU
 * emulation is going to explain away. Best-effort: on failure the caller omits
 * the property and the boot carries on, as it always did. */
static bool boot_rng_seed(void *out, uint32_t len) {
  if (getrandom(out, len, 0) == (ssize_t)len) {
    return true;
  }
  const int fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0) {
    return false;
  }
  unsigned char *const bytes = (unsigned char *)out;
  uint32_t got = 0U;
  while (got < len) {
    const ssize_t n = read(fd, bytes + got, (size_t)(len - got));
    if (n <= 0) {
      break;
    }
    got += (uint32_t)n;
  }
  (void)close(fd);
  return got == len;
}

/* For `-serial stdio`: put stdin in non-blocking raw mode so the run loop can
 * drain typed bytes into the UART RX ring between slices without ever blocking
 * the vCPU. Best-effort -- a redirected or closed stdin just yields EOF. */
static void boot_arm_stdin(void) {
  const int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
  if (flags >= 0) {
    (void)fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
  }
  struct termios t;
  if (tcgetattr(STDIN_FILENO, &t) == 0) {
    /* Raw by hand -- cfmakeraw needs GNU extensions the -std=c11 build hides.
     * No canonical buffering, no host echo, no flow control: every typed byte
     * reaches the run loop's read() as-is. */
    t.c_iflag &= (tcflag_t) ~(BRKINT | ICRNL | INLCR | INPCK | ISTRIP | IXON | IXOFF);
    t.c_oflag &= (tcflag_t)~OPOST;
    t.c_lflag &= (tcflag_t) ~(ECHO | ECHOE | ECHONL | ICANON | IEXTEN);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 0;
    (void)tcsetattr(STDIN_FILENO, TCSANOW, &t);
  }
}

/* Where the initrd goes: the three-quarter mark of RAM. Returns 0 (a signal * the caller turns
 * into an error) when the ramdisk, plus a margin for the boot stack and kernel heap, would not
 * fit between that mark and the top of RAM. */
static uint64_t boot_initrd_addr(uint64_t ram, uint64_t len) {
  const uint64_t base = BOOT_RAM_BASE + (ram / 4U) * 3U;
  if ((base + len + BOOT_INITRD_MARGIN) > (BOOT_RAM_BASE + ram)) {
    return 0U;
  }
  return base;
}

/*
 * Boots an AArch64 Linux Image at EL1 with the M4a boot protocol:
 * Image-loader placement, DTB in x0, a PL011 at virt's UART0 address, and
 * a PSCI conduit whose SYSTEM_OFF exits the process 0. `ram_mib` sizes the
 * RAM window; the DTB's memory node is patched to match, so `-m` never
 * makes the tree lie about the machine.
 */
static int boot(const boot_opts *opts) {
  oemu_buffer image;
  oemu_machine machine = {0};
  oemu_vcpu vcpu = {0};
  oemu_env_ops env = {0};
  oemu_memops bus = {0};
  oemu_pl011 uart = {0};
  oemu_gicv2 gic = {0};
  oemu_psci psci = {0};
  boot_env benv = {0};
  boot_serial ser = {0};
  oemu_image hdr = {0};
  oemu_buffer initrd = {0};
  oemu_fdt gen = {0};
  unsigned char *dtb = NULL;        /* the -dtb blob we own and may patch */
  const unsigned char *tree = NULL; /* the blob as placed on the bus (read-only view) */
  size_t dtb_len = 0U;
  size_t len = 0U;
  FILE *serial = NULL;
  const uint64_t ram = opts->ram_mib * (1U << 20);
  uint64_t entry = 0U;
  uint64_t dtb_pa = 0U;
  uint64_t initrd_pa = 0U;
  size_t initrd_len = 0U;
  bool generated = (opts->dtb == NULL); /* no -dtb -> build the virt tree ourselves */
  bool have_initrd = (opts->initrd != NULL);
  oemu_status st = OEMU_OK;
  int result = EXIT_ERROR;

  if (oemu_buffer_init(&image, 0U) != OEMU_OK) {
    (void)fputs("oemu: out of memory\n", stderr);
    return EXIT_ERROR;
  }
  if (read_file(opts->kernel, &image) != 0) {
    goto done;
  }
  len = oemu_buffer_len(&image);
  if (have_initrd) {
    if (oemu_buffer_init(&initrd, 0U) != OEMU_OK) {
      (void)fputs("oemu: out of memory\n", stderr);
      goto done;
    }
    if (read_file(opts->initrd, &initrd) != 0) {
      goto done;
    }
    initrd_len = oemu_buffer_len(&initrd);
  }
  if (generated) {
    /* Build the device tree in-process (oemu/fdt): the only way to hand the
     * guest a /chosen with linux,initrd-start/end without an offline dtc. The
     * -append command line lands in /chosen/bootargs, the property the kernel
     * actually reads -- unlike the fixture's decoy /chosen/bootline. */
    if (oemu_fdt_init(&gen, BOOT_DTB_MAX) != OEMU_OK) {
      (void)fputs("oemu: out of memory building the device tree\n", stderr);
      goto done;
    }
    if (have_initrd) {
      initrd_pa = boot_initrd_addr(ram, (uint64_t)initrd_len);
      if (initrd_pa == 0U) {
        (void)fprintf(
            stderr, "oemu: -initrd of %zu bytes does not fit the -m %" PRIu64 " MiB machine\n",
            initrd_len, opts->ram_mib);
        goto done;
      }
    }
    /* 32 bytes, the size the oracle injects. */
    uint8_t rng_seed[32];
    const bool seeded = boot_rng_seed(rng_seed, (uint32_t)sizeof(rng_seed));
    if (!seeded) {
      (void)fputs(
          "oemu: no host entropy for /chosen/rng-seed; the guest CRNG will stay unseeded\n",
          stderr);
    }
    const oemu_virt_dtb_params vp = {BOOT_RAM_BASE,
                                     ram,
                                     initrd_pa,
                                     initrd_pa + (uint64_t)initrd_len,
                                     opts->cmdline,
                                     seeded ? rng_seed : NULL,
                                     seeded ? (uint32_t)sizeof(rng_seed) : 0U};
    st = oemu_virt_dtb_build(&gen, &vp);
    if (st != OEMU_OK) {
      (void)fprintf(stderr, "oemu: building the device tree failed: %s\n", oemu_status_str(st));
      goto done;
    }
    tree = oemu_fdt_bytes(&gen);
    dtb_len = oemu_fdt_length(&gen);
  } else {
    if (dtb_load(opts->dtb, &dtb, &dtb_len) != 0) {
      (void)fprintf(stderr, "oemu: could not load the device tree\n");
      goto done;
    }
    if ((dtb_len < 40U) || (be32(dtb) != BOOT_FDT_MAGIC) || (be32(dtb + 4U) > dtb_len) ||
        (dtb_len > BOOT_DTB_MAX)) {
      (void)fprintf(stderr, "oemu: %s is not a valid device tree blob\n", opts->dtb);
      goto done;
    }
    if (opts->cmdline != NULL) {
      if (!dtb_patch_bootline(dtb, dtb_len, opts->cmdline)) {
        (void)fprintf(stderr, "oemu: -append: this DTB has no /chosen/bootline to write\n");
        goto done;
      }
    }
    /* The tree's /memory reg must describe the RAM we actually build, or a
     * guest trusts a lie. A tree with no /memory node asserts nothing, so warn
     * and carry on. */
    if (!dtb_patch_node(dtb, dtb_len, BOOT_RAM_BASE, ram)) {
      (void)fprintf(stderr, "oemu: note: the device tree has no /memory node to size\n");
    }
    if (have_initrd) {
      (void)fprintf(stderr,
                    "oemu: note: -initrd needs the generated tree; drop -dtb to inject it\n");
    }
    tree = dtb;
  }

  /* booting.rst: magic, min-version, flags, and the text fitting RAM. */
  st = oemu_image_parse_header(oemu_buffer_data(&image), &hdr);
  if (st != OEMU_OK) {
    (void)fprintf(stderr, "oemu: %s: %s\n", opts->kernel, oemu_status_str(st));
    goto done;
  }
  entry = (opts->entry != 0U) ? opts->entry : BOOT_RAM_BASE + hdr.text_offset;
  if ((entry & 3U) != 0U) {
    (void)fprintf(stderr, "oemu: entry 0x%" PRIx64 " is not 4-aligned\n", entry);
    goto done;
  }
  /* A caller-supplied entry must be somewhere the machine can fetch from.
   * Kernel text always lands in RAM, so refuse an entry outside it rather
   * than set the program counter at an address no region owns and let the
   * guest die on a Data/Instruction Abort it can never name. */
  if ((entry < BOOT_RAM_BASE) || (entry >= BOOT_RAM_BASE + ram)) {
    (void)fprintf(stderr, "oemu: entry 0x%" PRIx64 " lies outside the machine's RAM\n", entry);
    goto done;
  }

  if (opts->serial_path != NULL) {
    serial = fopen(opts->serial_path, "wb");
    if (serial == NULL) {
      (void)fprintf(stderr, "oemu: cannot open serial log '%s'\n", opts->serial_path);
      goto done;
    }
  } else {
    serial = stdout;
  }

  st = oemu_machine_init(&machine, BOOT_RAM_BASE, ram, BOOT_REGION_CAPACITY);
  if (st != OEMU_OK) {
    (void)fprintf(stderr, "oemu: machine init failed: %s\n", oemu_status_str(st));
    goto done;
  }
  ser.out = serial;
  oemu_pl011_init(&uart, &boot_serial_sink, &ser);
  st = oemu_aspace_attach_device(&machine.aspace, BOOT_UART_BASE, BOOT_UART_SIZE, &uart.ops);
  if (st != OEMU_OK) {
    (void)fprintf(stderr, "oemu: attaching the boot UART failed: %s\n", oemu_status_str(st));
    goto done;
  }
  /* The DT's /interrupt-controller node promises a GICv2 (distributor at
   * 0x08000000, CPU interface at 0x08010000), and init_IRQ holds the kernel to
   * that promise -- without it gic_of_init aborts and the whole boot dies. A
   * single-CPU secure model, 64 lines so SPI 32..63 covers the DT's sources. */
  oemu_gicv2_init(&gic, BOOT_GIC_LINES);
  st = oemu_aspace_attach_device(&machine.aspace, BOOT_GIC_DIST_BASE, BOOT_GIC_DIST_SIZE,
                                 &gic.dist_ops);
  if (st != OEMU_OK) {
    (void)fprintf(stderr, "oemu: attaching the GIC distributor failed: %s\n",
                  oemu_status_str(st));
    goto done;
  }
  st = oemu_aspace_attach_device(&machine.aspace, BOOT_GIC_CPU_BASE, BOOT_GIC_CPU_SIZE,
                                 &gic.cpu_ops);
  if (st != OEMU_OK) {
    (void)fprintf(stderr, "oemu: attaching the GIC CPU interface failed: %s\n",
                  oemu_status_str(st));
    goto done;
  }
  /* x0 will carry this address; the DTB lives in plain RAM at the halfway
   * mark -- inside the machine, past any kernel a -m 1024 guest unpacks. */
  dtb_pa = BOOT_RAM_BASE + (ram / 2U);
  bus = oemu_aspace_memops(&machine.aspace);

  /* Text lands at mem_base + text_offset per booting.rst, through the bus. */
  st = oemu_image_load(&hdr, oemu_buffer_data(&image), len, &bus, BOOT_RAM_BASE, ram);
  if (st != OEMU_OK) {
    (void)fprintf(stderr, "oemu: loading %s failed: %s\n", opts->kernel, oemu_status_str(st));
    goto done;
  }
  /* The tree rides the bus too, byte by byte, at the halfway mark. */
  {
    uint64_t pa = dtb_pa;
    for (size_t i = 0U; i < dtb_len; i++, pa++) {
      st = oemu_aspace_write(&machine.aspace, pa, OEMU_MEM_BYTE, tree[i]);
      if (st != OEMU_OK) {
        (void)fprintf(stderr, "oemu: placing the DTB failed: %s\n", oemu_status_str(st));
        goto done;
      }
    }
  }
  /* The ramdisk lands where the generated tree promised it. The address was
   * checked to fit at build time, so a bus write only fails on a real fault. */
  if (have_initrd) {
    const unsigned char *ib = oemu_buffer_data(&initrd);
    for (uint64_t pa = initrd_pa, i = 0U; i < (uint64_t)initrd_len; i++, pa++) {
      st = oemu_aspace_write(&machine.aspace, pa, OEMU_MEM_BYTE, ib[i]);
      if (st != OEMU_OK) {
        (void)fprintf(stderr, "oemu: placing the initrd failed: %s\n", oemu_status_str(st));
        goto done;
      }
    }
  }

  oemu_psci_init(&psci);
  benv.machine = &machine;
  benv.psci = &psci;
  env.ctx = &benv;
  env.syscall = NULL;
  env.halted = &boot_halted;
  env.fw_call = &boot_fw_call;
  st = oemu_vcpu_init(&vcpu, &bus, &env, OEMU_EL1, entry, BOOT_RAM_BASE + ram - 16U,
                      BOOT_QUANTUM);
  if (st != OEMU_OK) {
    (void)fprintf(stderr, "oemu: vcpu init failed: %s\n", oemu_status_str(st));
    goto done;
  }
  /* The boot protocol: x0 carries the DTB's physical address. */
  oemu_regs_write(&vcpu.cpu.regs, 0U, OEMU_REG_W64, dtb_pa);
  if (opts->stdio) {
    boot_arm_stdin();
  }
  result = boot_run(&vcpu, &machine, &uart, &gic, opts->stdio, opts->max_insns);
  { /* TEMPORARY diagnostic (issue #28): OEMU_DUMP_MEM=pa:size:file dumps guest RAM
     * at exit, so a suspicion about what the guest did to a page becomes a fact. */
    const char *m = getenv("OEMU_DUMP_MEM");
    if (m != NULL) {
      /* OEMU_DUMP_MEM=<pa>:<len>:<file>, both hex, to look at guest RAM at a chosen
       * moment. Parsed with strtoull rather than sscanf("%lx"): the conversion
       * must be checked, the values are uint64_t (no room for an unsigned-long
       * cast on ILP32), and a hex field is not something sscanf may read past
       * whitespace for. */
      char *end = NULL;
      uint64_t base = 0ULL;
      uint64_t size = 0ULL;
      char path[256];
      const char *colon1 = (m != NULL) ? strchr(m, ':') : NULL;
      const char *colon2 = (colon1 != NULL) ? strchr(colon1 + 1, ':') : NULL;
      errno = 0;
      base = (m != NULL) ? strtoull(m, &end, 16) : 0ULL;
      size = (end == colon1) ? strtoull(colon1 + 1, &end, 16) : 0ULL;
      if ((errno == 0) && (colon2 != NULL) && (end == colon2) &&
          (sscanf(colon2 + 1, "%255s", path) == 1) && (size != 0ULL)) {
        FILE *f = fopen(path, "wb");
        if (f != NULL) {
          for (uint64_t off = 0ULL; off < size; off++) {
            uint64_t b = 0ULL;
            if (oemu_aspace_read(&machine.aspace, base + off, OEMU_MEM_BYTE, false, &b) !=
                OEMU_OK) {
              b = 0xDEULL;
            }
            (void)fputc((int)(b & 0xFFULL), f);
          }
          (void)fclose(f);
        }
      }
    }
  }
  (void)oemu_pl011_pump(&uart); /* whatever the guest queued before it died */

done:
  if ((serial != NULL) && (serial != stdout)) {
    (void)fclose(serial);
  }
  /* The generated tree is owned by the fdt builder (dtb points into it); an
   * -dtb blob is a lone allocation from dtb_load. Two different owners. */
  if (generated) {
    oemu_fdt_dispose(&gen);
  } else if (dtb != NULL) {
    oemu_allocator_get()->free(dtb, NULL);
  }
  if (have_initrd) {
    oemu_buffer_dispose(&initrd);
  }
  oemu_machine_dispose(&machine);
  oemu_buffer_dispose(&image);
  return result;
}

static int parse_max_insns(const char *text, uint64_t *out) {
  errno = 0;
  char *end = NULL;
  const unsigned long long value = strtoull(text, &end, 10);
  if (end == text || *end != '\0' || errno != 0) {
    (void)fprintf(stderr, "oemu: invalid --max-insns '%s'\n", text);
    return -1;
  }
  *out = (uint64_t)value;
  return 0;
}

/* An entry address, written as C writes it: 0x-prefixed hex or plain decimal.
 * A leading '-' is refused -- strtoull would wrap it into a huge address that
 * then passes no sensible range check. */
/* RAM size in MiB, decimal and positive: `-m 0` would build no machine. */
static int parse_mib(const char *text, uint64_t *out) {
  errno = 0;
  char *end = NULL;
  const unsigned long long value = strtoull(text, &end, 10);
  if (end == text || *end != '\0' || errno != 0 || value == 0ULL || value > (1ULL << 32)) {
    (void)fprintf(stderr, "oemu: invalid -m '%s'\n", text);
    return -1;
  }
  *out = (uint64_t)value;
  return 0;
}

/* Today the only --serial destination form is `file:PATH`, matching the
 * oracle's `-serial file:...` spelling one for one. */
static int parse_serial(const char *text, const char **path_out) {
  static const char prefix[] = "file:";
  if (strncmp(text, prefix, sizeof(prefix) - 1U) != 0 || text[sizeof(prefix) - 1U] == '\0') {
    (void)fprintf(stderr, "oemu: --serial wants file:PATH (got '%s')\n", text);
    return -1;
  }
  *path_out = text + sizeof(prefix) - 1U;
  return 0;
}

static int parse_entry(const char *text, uint64_t *out) {
  errno = 0;
  char *end = NULL;
  const unsigned long long value = strtoull(text, &end, 0);
  if (end == text || *end != '\0' || errno != 0 || text[0] == '-') {
    (void)fprintf(stderr, "oemu: invalid --entry '%s'\n", text);
    return -1;
  }
  *out = (uint64_t)value;
  return 0;
}

int main(int argc, char **argv) {
  if (argc >= 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
    print_usage(stdout);
    return 0;
  }
  if (argc >= 2 && strcmp(argv[1], "boot") == 0) {
    /* `-kernel` is mandatory and must carry its argument: an implicit kernel
     * path would boot whatever the shell remembered last. */
    if (argc < 4 || strcmp(argv[2], "-kernel") != 0) {
      print_usage(stderr);
      return EXIT_USAGE;
    }
    boot_opts opts = {0};
    opts.kernel = argv[3];
    opts.ram_mib = BOOT_RAM_DEFAULT;
    opts.max_insns = UINT64_MAX;
    for (int i = 4; i < argc; i++) {
      const char *a = argv[i];
      const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
      if (strcmp(a, "-append") == 0 || strcmp(a, "-dtb") == 0) {
        if (v == NULL) {
          print_usage(stderr);
          return EXIT_USAGE;
        }
        if (a[1] == 'a') {
          opts.cmdline = v;
        } else {
          opts.dtb = v;
        }
        i++;
      } else if (strcmp(a, "-initrd") == 0) {
        if (v == NULL) {
          print_usage(stderr);
          return EXIT_USAGE;
        }
        opts.initrd = v;
        i++;
      } else if (strcmp(a, "-m") == 0) {
        if (v == NULL || parse_mib(v, &opts.ram_mib) != 0) {
          print_usage(stderr);
          return EXIT_USAGE;
        }
        i++;
      } else if (strcmp(a, "--serial") == 0 || strcmp(a, "-serial") == 0) {
        if (v == NULL) {
          print_usage(stderr);
          return EXIT_USAGE;
        }
        if (strcmp(v, "stdio") == 0) {
          opts.stdio = true; /* console to stdout, and stdin feeds the RX ring */
        } else if (parse_serial(v, &opts.serial_path) != 0) {
          print_usage(stderr);
          return EXIT_USAGE;
        }
        i++;
      } else if (strcmp(a, "--entry") == 0) {
        if (v == NULL || parse_entry(v, &opts.entry) != 0) {
          print_usage(stderr);
          return EXIT_USAGE;
        }
        i++;
      } else if (strcmp(a, "--max-insns") == 0) {
        if (v == NULL || parse_max_insns(v, &opts.max_insns) != 0) {
          print_usage(stderr);
          return EXIT_USAGE;
        }
        i++;
      } else if (strcmp(a, "--smp") == 0) {
        (void)fprintf(stderr,
                      "oemu: --smp is not supported yet; one vCPU boots alone (M4b+)\n");
        return EXIT_USAGE;
      } else if (strcmp(a, "-smp") == 0) {
        if (v == NULL) {
          print_usage(stderr);
          return EXIT_USAGE;
        }
        (void)fprintf(stderr, "oemu: -smp is not supported yet; one vCPU boots alone (M4b+)\n");
        return EXIT_USAGE;
      } else {
        (void)fprintf(stderr, "oemu: unknown option '%s'\n", a);
        print_usage(stderr);
        return EXIT_USAGE;
      }
    }
    return boot(&opts);
  }
  if (argc < 3 || strcmp(argv[1], "run") != 0) {
    print_usage(stderr);
    return EXIT_USAGE;
  }

  const char *path = argv[2];
  uint64_t max_insns = UINT64_MAX; /* no budget: run until the guest exits */
  for (int i = 3; i < argc; i++) {
    if (strcmp(argv[i], "--max-insns") == 0) {
      if (i + 1 >= argc || parse_max_insns(argv[i + 1], &max_insns) != 0) {
        print_usage(stderr);
        return EXIT_USAGE;
      }
      i++;
    } else {
      (void)fprintf(stderr, "oemu: unknown option '%s'\n", argv[i]);
      print_usage(stderr);
      return EXIT_USAGE;
    }
  }
  return run(path, max_insns);
}
