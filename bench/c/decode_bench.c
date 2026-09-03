/*
 * Decode-throughput microbenchmark.
 *
 * Feeds the corpus blobs -- real AArch64 code cross-compiled by
 * bench/corpus/gen.sh, .text extracted as raw words -- through
 * oemu_decode() and reports nanoseconds per decoded instruction.
 *
 * Deliberately a plain C11 main(): a benchmark is a consumer of the library,
 * not part of it, and not a test either. Pulling google-benchmark in would
 * add a system dependency the project has managed without so far; this file
 * needs a clock, a directory listing and printf.
 *
 * Methodology notes that matter for interpreting the numbers:
 *   - Calibration runs one timed pass, then chooses an iteration count so a
 *     full batch lands near --budget milliseconds. Batch sizes differ per
 *     blob; the reported unit (ns/insn) normalises that away.
 *   - --trials batches per blob, report the minimum: the minimum is the
 *     estimator that survives frequency scaling and a noisy neighbour.
 *   - The corpus is deterministic, so a run-to-run minimum spread larger
 *     than a few percent means the machine, not the code, changed.
 */
#define _POSIX_C_SOURCE 200809L

#include "oemu/decode.h"
#include "oemu/status.h"

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef OEMU_BENCH_CORPUS_DIR
#define OEMU_BENCH_CORPUS_DIR "bench/corpus/blob"
#endif

#define BLOB_NAME_MAX 128
#define SUFFIX        ".bin"

typedef struct blob {
  char name[BLOB_NAME_MAX];
  uint32_t *words; /* little-endian instruction words, heap-owned */
  size_t word_count;
  double best_ns_per_insn;
} blob;

static struct {
  uint64_t ok;
  uint64_t decode;
  uint64_t unsupported;
  uint64_t other;
} g_status_tally;

static volatile uint64_t g_sink;

static void usage(const char *program);
static uint64_t monotonic_ns(void);
static uint64_t run_pass(const uint32_t *words, size_t word_count, bool tally);
static double bench_blob(blob *b, unsigned trials, double budget_ns);
static uint32_t *read_blob_words(const char *path, size_t *out_count);
static int compare_blob_names(const void *lhs, const void *rhs);
static bool has_suffix(const char *name, const char *suffix);

static void usage(const char *program) {
  (void)printf("usage: %s [--corpus DIR] [--trials N] [--budget MS]\n", program);
  (void)printf("  corpus default: %s (override with $OEMU_BENCH_CORPUS)\n",
               OEMU_BENCH_CORPUS_DIR);
}

static uint64_t monotonic_ns(void) {
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    (void)fputs("decode-bench: clock_gettime failed\n", stderr);
    exit(2);
  }
  return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* One full pass over one blob. Returns a checksum-style sink so the decode
 * loop cannot be optimised into anything other than the calls it makes. */
static uint64_t run_pass(const uint32_t *words, size_t word_count, bool tally) {
  oemu_insn insn;
  uint64_t sink = 0;

  for (size_t i = 0; i < word_count; i++) {
    oemu_status status = oemu_decode(words[i], 0x400000ull + (uint64_t)i * 4ull, &insn);

    /* Mix in decoded identity and the status: a decoder that starts
     * returning one cached answer for everything would score the same on
     * time but diverge on the status tallies printed at the end. */
    sink +=
        (uint64_t)insn.op + (uint64_t)(uint32_t)insn.imm + (uint64_t)insn.rd + (uint64_t)status;

    if (tally) {
      switch (status) {
        case OEMU_OK:
          g_status_tally.ok++;
          break;
        case OEMU_ERR_DECODE:
          g_status_tally.decode++;
          break;
        case OEMU_ERR_UNSUPPORTED:
          g_status_tally.unsupported++;
          break;
        /* Listed so -Wswitch-enum is satisfied even with a default present;
         * the library contract is that decode returns one of the three above,
         * and anything else still lands in `other`. */
        case OEMU_ERR_INVALID_ARG:
        case OEMU_ERR_NO_MEMORY:
        case OEMU_ERR_OVERFLOW:
        case OEMU_ERR_RANGE:
        case OEMU_ERR_FAULT:
        case OEMU_ERR_TIMEOUT:
        default:
          g_status_tally.other++;
          break;
      }
    }
  }
  return sink;
}

/* Calibrate, then run `trials` batches; store and return the minimum
 * ns/insn. */
static double bench_blob(blob *b, unsigned trials, double budget_ns) {
  uint64_t calib_t0 = monotonic_ns();
  uint64_t calib_sink = run_pass(b->words, b->word_count, true);
  double pass_ns = (double)(monotonic_ns() - calib_t0);

  uint64_t iters = 1u;
  if (pass_ns > 0.0) {
    iters = (uint64_t)(budget_ns / pass_ns);
  }
  if (iters < 4u) {
    iters = 4u;
  }
  if (iters > 20000000ull) {
    iters = 20000000ull;
  }

  g_sink = calib_sink; /* keep the calibrated pass observable to optimiser */

  double best = -1.0;
  for (unsigned trial = 0; trial < trials; trial++) {
    uint64_t t0 = monotonic_ns();
    for (uint64_t iter = 0; iter < iters; iter++) {
      g_sink = run_pass(b->words, b->word_count, false);
    }
    uint64_t batch_ns = monotonic_ns() - t0;
    double ns_per_insn = (double)batch_ns / ((double)b->word_count * (double)iters);

    if (best < 0.0 || ns_per_insn < best) {
      best = ns_per_insn;
    }
  }

  b->best_ns_per_insn = best;
  return best;
}

static bool has_suffix(const char *name, const char *suffix) {
  size_t name_len = strlen(name);
  size_t suffix_len = strlen(suffix);

  return name_len >= suffix_len &&
         memcmp(name + (name_len - suffix_len), suffix, suffix_len) == 0;
}

static int compare_blob_names(const void *lhs, const void *rhs) {
  const blob *a = (const blob *)lhs;
  const blob *b = (const blob *)rhs;

  return strcmp(a->name, b->name);
}

/* Reads `path`, truncates a trailing partial word, and returns it as
 * host-order uint32 (the corpus is little-endian; so is every supported
 * host, but the words are assembled from bytes to keep it honest). */
static uint32_t *read_blob_words(const char *path, size_t *out_count) {
  int fd = open(path, O_RDONLY);
  struct stat st;
  size_t size;
  size_t filled = 0;
  uint8_t *buf;
  uint32_t *words;

  if (fd < 0) {
    (void)fprintf(stderr, "decode-bench: %s: %s\n", path, strerror(errno));
    return NULL;
  }
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 4) {
    (void)close(fd);
    return NULL;
  }

  size = (size_t)st.st_size;
  size -= size % 4u;
  buf = (uint8_t *)malloc(size);
  if (buf == NULL) {
    (void)close(fd);
    return NULL;
  }

  while (filled < size) {
    ssize_t chunk = read(fd, buf + filled, size - filled);
    if (chunk <= 0) {
      (void)close(fd);
      free(buf);
      return NULL;
    }
    filled += (size_t)chunk;
  }
  (void)close(fd);

  words = (uint32_t *)malloc(size);
  if (words == NULL) {
    free(buf);
    return NULL;
  }
  for (size_t i = 0; i < size / 4u; i++) {
    const uint8_t *p = buf + i * 4u;
    words[i] = (uint32_t)p[0] | ((uint32_t)p[1] << 8u) | ((uint32_t)p[2] << 16u) |
               ((uint32_t)p[3] << 24u);
  }
  free(buf);

  *out_count = size / 4u;
  return words;
}

int main(int argc, char **argv) {
  const char *corpus_dir = getenv("OEMU_BENCH_CORPUS");
  unsigned trials = 5u;
  double budget_ns = 150.0e6;
  DIR *dir;
  blob *blobs;
  size_t blob_capacity = 64u;
  size_t blob_count = 0u;
  struct dirent *ent;

  for (int arg = 1; arg < argc; arg++) {
    const char *flag = argv[arg];

    if (strcmp(flag, "--help") == 0) {
      usage(argv[0]);
      return 0;
    } else if (strcmp(flag, "--corpus") == 0 && arg + 1 < argc) {
      corpus_dir = argv[++arg];
    } else if (strcmp(flag, "--trials") == 0 && arg + 1 < argc) {
      unsigned long parsed = strtoul(argv[++arg], NULL, 10);
      trials = (parsed >= 1u && parsed <= 1000u) ? (unsigned)parsed : trials;
    } else if (strcmp(flag, "--budget") == 0 && arg + 1 < argc) {
      unsigned long parsed = strtoul(argv[++arg], NULL, 10);
      if (parsed >= 10u && parsed <= 10000u) {
        budget_ns = (double)parsed * 1.0e6;
      }
    } else {
      usage(argv[0]);
      return 1;
    }
  }

  if (corpus_dir == NULL) {
    corpus_dir = OEMU_BENCH_CORPUS_DIR;
  }

  blobs = (blob *)calloc(blob_capacity, sizeof(blob));
  if (blobs == NULL) {
    (void)fputs("decode-bench: out of memory\n", stderr);
    return 2;
  }

  dir = opendir(corpus_dir);
  if (dir == NULL) {
    (void)fprintf(stderr, "decode-bench: cannot open corpus directory '%s': %s\n", corpus_dir,
                  strerror(errno));
    free(blobs);
    return 2;
  }

  while ((ent = readdir(dir)) != NULL) {
    char path[4096];
    size_t name_len = strlen(ent->d_name);
    size_t word_count = 0u;
    uint32_t *words;

    if (!has_suffix(ent->d_name, SUFFIX)) {
      continue;
    }
    if (name_len >= BLOB_NAME_MAX) {
      (void)fprintf(stderr, "decode-bench: skipping over-long name %s\n", ent->d_name);
      continue;
    }
    /* The precision caps make the truncation provably impossible: the name
     * is bounded above by BLOB_NAME_MAX - 1, the directory by the width, so
     * no -Wformat-truncation diagnostic can fire and nothing is silently cut. */
    (void)snprintf(path, sizeof(path), "%.3900s/%.127s", corpus_dir, ent->d_name);

    words = read_blob_words(path, &word_count);
    if (words == NULL || word_count == 0u) {
      free(words);
      (void)fprintf(stderr, "decode-bench: skipping unusable blob %s\n", path);
      continue;
    }

    if (blob_count == blob_capacity) {
      blob *grown;
      blob_capacity *= 2u;
      grown = (blob *)realloc(blobs, blob_capacity * sizeof(blob));
      if (grown == NULL) {
        (void)fputs("decode-bench: out of memory\n", stderr);
        free(blobs);
        (void)closedir(dir);
        return 2;
      }
      blobs = grown;
    }
    /* memcpy of a verified length rather than strncpy: exact and immune to
     * the truncation diagnostic the bounded copy would trigger. */
    (void)memcpy(blobs[blob_count].name, ent->d_name, name_len + 1u);
    blobs[blob_count].words = words;
    blobs[blob_count].word_count = word_count;
    blobs[blob_count].best_ns_per_insn = 0.0;
    blob_count++;
  }
  (void)closedir(dir);

  if (blob_count == 0u) {
    (void)fprintf(stderr, "decode-bench: no .bin blobs in '%s'; run bench/corpus/gen.sh\n",
                  corpus_dir);
    free(blobs);
    return 2;
  }

  qsort(blobs, blob_count, sizeof(blob), compare_blob_names);

  (void)printf("oemu decode benchmark\n");
  (void)printf("corpus: %s  trials: %u  budget: %.0f ms/blob\n\n", corpus_dir, trials,
               budget_ns / 1.0e6);
  (void)printf("%-28s %10s %10s %10s\n", "blob", "words", "ns/insn", "Mword/s");

  {
    double total_time_ns = 0.0;
    size_t total_words = 0u;

    for (size_t i = 0; i < blob_count; i++) {
      double ns_per_insn = bench_blob(&blobs[i], trials, budget_ns);
      double mops = (ns_per_insn > 0.0) ? (1000.0 / ns_per_insn) : 0.0;

      total_time_ns += ns_per_insn * (double)blobs[i].word_count;
      total_words += blobs[i].word_count;

      (void)printf("%-28s %10zu %10.3f %10.2f\n", blobs[i].name, blobs[i].word_count,
                   ns_per_insn, mops);
    }

    (void)printf("%-28s %10zu %10.3f %10.2f\n", "-- total --", total_words,
                 total_time_ns / (double)total_words,
                 (double)total_words * 1000.0 / total_time_ns);
  }

  (void)printf("\nstatuses (first pass, all blobs): ok=%" PRIu64 " decode_err=%" PRIu64
               " unsupported=%" PRIu64 " other=%" PRIu64 "\n",
               g_status_tally.ok, g_status_tally.decode, g_status_tally.unsupported,
               g_status_tally.other);
  (void)printf(
      "interpretation: ns/insn is the min-of-trials decode cost for one A64 word\n"
      "of real compiler output; decode_err/unsupported must stay 0 for a pure\n"
      "integer corpus -- a non-zero count is a decode regression signal.\n");

  for (size_t i = 0; i < blob_count; i++) {
    free(blobs[i].words);
  }
  free(blobs);
  return 0;
}
