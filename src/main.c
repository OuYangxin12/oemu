/*
 * The oemu command line.
 *
 *   oemu run <image.elf> [--max-insns N]
 *   oemu boot -kernel <raw-bin> [--entry ADDR] [--max-insns N]
 *   oemu --help
 *
 * `run` boots a static AArch64 ET_EXEC image: read the file, hand it to
 * oemu_elf_load, give the guest a stack (the loader deliberately does not
 * synthesise one), run it, and exit with the status code the guest asked for.
 * The guest's own stdout passes straight through, so
 * `oemu run prog.elf | diff - expected.txt` works.
 *
 * `boot` (M2c) starts a raw AArch64 image at EL1 with identity mapping -- the
 * kernel-boot protocol path. The machine layout mirrors the QEMU virt oracle
 * baseline (docs/linux-minimal-qemu.md) so the same guest runs on both: RAM
 * 256 MiB at 0x40000000, the image at 0x40080000 (the AArch64 Image load
 * address; the header's `b _start` makes the load address a valid entry point,
 * so --entry defaults to it), and a stopgap UART at 0x09000000 (QEMU virt's
 * UART0 -- a temporary device only until M4a's real PL011 replaces it; the
 * write path emits one byte to stdout, every register reads 0). x0..x3 come in
 * zero: x0=DTB pointer is M4a's boot protocol, and there is no DTB yet.
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
#include "oemu/machine.h"
#include "oemu/memory.h"
#include "oemu/status.h"
#include "oemu/sysenv.h"
#include "oemu/vcpu.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#define BOOT_RAM_BASE   ((uint64_t)0x40000000ULL)
#define BOOT_RAM_SIZE   ((uint64_t)0x10000000ULL) /* 256 MiB, as the oracle baseline */
#define BOOT_IMAGE_BASE ((uint64_t)0x40080000ULL) /* booting.rst kernel load address */
#define BOOT_UART_BASE  ((uint64_t)0x09000000ULL) /* virt UART0 -- oracle-portable */
#define BOOT_UART_SIZE  ((uint64_t)0x00001000ULL)
#define BOOT_UART_DR    ((uint64_t)0x00ULL) /* PL011 DR offset: the only register we honour */
#define BOOT_UART_EOT   (0x04U)             /* sentinel byte: the guest asks to stop */
#define BOOT_REGION_CAPACITY 8U             /* RAM + UART today; room for M4 devices */
/* Instructions per scheduler slice. One vCPU, so a quantum is purely the
 * latency bound between machine-event polls; 1M keeps a stuck guest inside
 * the 60 s test budget while keeping syscall-free slices cheap. */
#define BOOT_QUANTUM UINT64_C(1000000)

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
  (void)fputs("       oemu boot -kernel <raw-bin> [--entry ADDR] [--max-insns N]\n", out);
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
 * The stopgap boot console. A PL011 write to DR emits exactly the low byte;
 * every register reads 0, which for FR means "TX FIFO not full, not busy" --
 * the answer that keeps a real driver writing. Writes to other offsets are
 * ignored. This is deliberately the smallest thing that can carry the markers
 * tests assert on: M4a's src/dev/pl011.c replaces it wholesale, and the guest
 * code changes not one line because the addresses and the write path match.
 */
typedef struct boot_uart {
  oemu_machine *machine;
  FILE *out;
} boot_uart;

static oemu_status boot_uart_read(void *ctx, uint64_t offset, oemu_mem_size size,
                                  uint64_t *value_out) {
  (void)ctx;
  (void)offset;
  (void)size;
  *value_out = 0U;
  return OEMU_OK;
}

static oemu_status boot_uart_write(void *ctx, uint64_t offset, oemu_mem_size size,
                                   uint64_t value) {
  (void)size; /* stopgap: any width commits only its low byte */
  boot_uart *uart = (boot_uart *)ctx;
  if (offset != BOOT_UART_DR) {
    return OEMU_OK; /* documented: writes elsewhere are ignored */
  }
  const int byte = (int)(value & 0xFFU);
  if ((unsigned)byte == BOOT_UART_EOT) {
    /* The exit protocol: a sticky machine event, not a host exit -- the run
     * loop owns the process, the device only records the wish. */
    oemu_machine_poweroff(uart->machine, 0);
    return OEMU_OK;
  }
  (void)fputc(byte, uart->out);
  (void)fflush(uart->out); /* markers must be visible even if the run dies later */
  return OEMU_OK;
}

/* The env's `halted` mirrors the machine's sticky event, so oemu_vcpu_run
 * notices a device-raised powerdown at the very next instruction boundary.
 * `syscall` is NULL on purpose: at EL1 an SVC is an exception into the guest's
 * vectors, not a host call -- the vCPU never consults it in system mode. */
static bool boot_halted(const void *ctx) {
  return oemu_machine_event_peek((const oemu_machine *)ctx) != OEMU_MACHINE_EVENT_NONE;
}

/* Assembles the little-endian doubleword the device model expects from eight
 * host-buffer bytes, so the image copy does not depend on host byte order. */
static uint64_t load64le(const unsigned char *p) {
  uint64_t v = 0U;
  for (unsigned i = 0U; i < 8U; i++) {
    v |= (uint64_t)p[i] << (8U * i);
  }
  return v;
}

/* Copies the image into guest RAM through the bus itself (the machine
 * deliberately exposes no host pointer). Little-endian and 8-byte-chunked on
 * purpose, so a future big-endian host still boots the same image. */
static oemu_status boot_load_image(oemu_aspace *as, const unsigned char *src, size_t len) {
  uint64_t pa = BOOT_IMAGE_BASE;
  while (len >= 8U) {
    const oemu_status st =
        oemu_aspace_write(as, pa, OEMU_MEM_DWORD, load64le(src));
    if (st != OEMU_OK) {
      return st;
    }
    src += 8U;
    pa += 8U;
    len -= 8U;
  }
  for (; len > 0U; len--, src++, pa++) {
    const oemu_status st = oemu_aspace_write(as, pa, OEMU_MEM_BYTE, *src);
    if (st != OEMU_OK) {
      return st;
    }
  }
  return OEMU_OK;
}

/*
 * The cooperative run loop: one quantum at a time, then a look at the machine.
 * Every exit reason is decided here -- the guest asks through a device, the CLI
 * acts. A separate function so `boot` itself keeps the goto-clean shape the
 * project's -Wjump-misses-init demands of functions with a cleanup label.
 */
static int boot_run(oemu_vcpu *vcpu, oemu_machine *machine, uint64_t max_insns) {
  uint64_t budget = max_insns;
  for (;;) {
    const uint64_t slice = (budget < BOOT_QUANTUM) ? budget : BOOT_QUANTUM;
    uint64_t done = 0U;
    const oemu_status st = oemu_vcpu_run(vcpu, slice, &done);
    budget -= done;
    const oemu_machine_event ev = oemu_machine_event_peek(machine);
    if (ev == OEMU_MACHINE_EVENT_POWERDOWN) {
      return machine->exit_code & 0xFF; /* the code travels as a shell sees it */
    }
    if (ev == OEMU_MACHINE_EVENT_RESET) {
      (void)fputs("oemu: guest requested a reset, but restart is not implemented\n", stderr);
      return EXIT_ERROR;
    }
    if (st == OEMU_ERR_BLOCKED) {
      (void)fputs("oemu: guest parked at WFI/WFE with nothing to wake it\n", stderr);
      return EXIT_BLOCKED;
    }
    if (st != OEMU_OK && st != OEMU_ERR_TIMEOUT) {
      (void)fprintf(stderr, "oemu: boot failed: %s\n", oemu_status_str(st));
      return EXIT_ERROR;
    }
    if (budget == 0U) {
      (void)fprintf(stderr, "oemu: timeout after %" PRIu64 " instructions\n", max_insns);
      return EXIT_TIMEOUT;
    }
    oemu_vcpu_rearm(vcpu);
  }
}

/* Boots `kernel_path` as a raw image at EL1 and returns the process status. */
static int boot(const char *kernel_path, uint64_t entry, uint64_t max_insns) {
  oemu_buffer image;
  oemu_machine machine = {0};
  oemu_vcpu vcpu = {0};
  oemu_env_ops env = {0};
  oemu_memops bus = {0};
  boot_uart uart = {0};
  oemu_device_ops uart_ops = {0};
  oemu_status st = OEMU_OK;
  size_t len = 0U;
  int result = EXIT_ERROR;

  if (oemu_buffer_init(&image, 0U) != OEMU_OK) {
    (void)fputs("oemu: out of memory\n", stderr);
    return EXIT_ERROR;
  }
  if (read_file(kernel_path, &image) != 0) {
    goto done;
  }
  len = oemu_buffer_len(&image);
  if (len > (size_t)(BOOT_RAM_BASE + BOOT_RAM_SIZE - BOOT_IMAGE_BASE)) {
    (void)fprintf(stderr, "oemu: %s: image of %zu bytes does not fit below the RAM end\n",
                  kernel_path, len);
    goto done;
  }
  /* The architecture requires a 4-byte-aligned PC, and the entry must be
   * inside the machine -- an entry outside RAM is a caller error, not a
   * guest-visible Instruction Abort at an address no region owns. */
  if ((entry & 3U) != 0U || entry < BOOT_RAM_BASE ||
      entry >= BOOT_RAM_BASE + BOOT_RAM_SIZE) {
    (void)fprintf(stderr, "oemu: entry 0x%" PRIx64 " is not 4-aligned inside RAM\n", entry);
    goto done;
  }

  st = oemu_machine_init(&machine, BOOT_RAM_BASE, BOOT_RAM_SIZE, BOOT_REGION_CAPACITY);
  if (st != OEMU_OK) {
    (void)fprintf(stderr, "oemu: machine init failed: %s\n", oemu_status_str(st));
    goto done;
  }
  uart.machine = &machine;
  uart.out = stdout;
  uart_ops.ctx = &uart;
  uart_ops.read = boot_uart_read;
  uart_ops.write = boot_uart_write;
  st = oemu_aspace_attach_device(&machine.aspace, BOOT_UART_BASE, BOOT_UART_SIZE, &uart_ops);
  if (st != OEMU_OK) {
    (void)fprintf(stderr, "oemu: attaching the boot UART failed: %s\n", oemu_status_str(st));
    goto done;
  }

  st = boot_load_image(&machine.aspace, oemu_buffer_data(&image), len);
  if (st != OEMU_OK) {
    (void)fprintf(stderr, "oemu: loading the image failed: %s\n", oemu_status_str(st));
    goto done;
  }

  /* Entry stack hint: the top of RAM, 16-aligned. Linux guests set their own
   * SP before touching the stack; a smoke guest that relies on this lands in
   * zero-filled RAM either way. */
  env.ctx = &machine;
  env.syscall = NULL;
  env.halted = boot_halted;
  bus = oemu_aspace_memops(&machine.aspace);
  st = oemu_vcpu_init(&vcpu, &bus, &env, OEMU_EL1, entry,
                      BOOT_RAM_BASE + BOOT_RAM_SIZE - 16U, BOOT_QUANTUM);
  if (st != OEMU_OK) {
    (void)fprintf(stderr, "oemu: vcpu init failed: %s\n", oemu_status_str(st));
    goto done;
  }
  result = boot_run(&vcpu, &machine, max_insns);

done:
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
    uint64_t entry = BOOT_IMAGE_BASE; /* booting.rst: load address IS the entry point */
    uint64_t max_insns = UINT64_MAX;
    for (int i = 4; i < argc; i++) {
      if (strcmp(argv[i], "--entry") == 0) {
        if (i + 1 >= argc || parse_entry(argv[i + 1], &entry) != 0) {
          print_usage(stderr);
          return EXIT_USAGE;
        }
        i++;
      } else if (strcmp(argv[i], "--max-insns") == 0) {
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
    return boot(argv[3], entry, max_insns);
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
