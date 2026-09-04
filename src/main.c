/*
 * The oemu command line.
 *
 *   oemu run <image.elf> [--max-insns N]
 *   oemu --help
 *
 * Boots a static AArch64 ET_EXEC image: read the file, hand it to oemu_elf_load,
 * give the guest a stack (the loader deliberately does not synthesise one), run
 * it, and exit with the status code the guest asked for. The guest's own stdout
 * passes straight through, so `oemu run prog.elf | diff - expected.txt` works.
 *
 * This is the whole point of the loader: the step between "the executor runs a
 * hand-fed program" and "oemu boots a binary from disk".
 *
 * Diagnostics go to stderr and the guest's output to stdout, kept strictly apart
 * so the pass-through stream stays byte-clean. Only the fixed read chunk lives on
 * the stack; the image itself is held by oemu_buffer, whose allocation goes
 * through the library's allocator seam like everything else in src/.
 */
#include "oemu/buffer.h"
#include "oemu/elf.h"
#include "oemu/exec.h"
#include "oemu/memory.h"
#include "oemu/status.h"
#include "oemu/sysenv.h"

#include <errno.h>
#include <inttypes.h>
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

int main(int argc, char **argv) {
  if (argc >= 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
    print_usage(stdout);
    return 0;
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
