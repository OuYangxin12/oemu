/*
 * Corpus kernel: loads and stores.
 *
 * Targets OEMU_OP_LDR / STR at every width and signedness, the post-index
 * pointer-walk form, SP-relative accesses through a large frame, the pair
 * forms OEMU_OP_LDP / STP (both the prologue/epilogue save-restore shape and
 * the paired-access shape), and unaligned scalar access through a packed
 * struct -- legal on AArch64 and a decode case worth having.
 */
#include "corpus_common.h"

struct rec {
  int64_t big;
  int32_t mid;
  int16_t small;
  int8_t tiny;
};

struct unaligned {
  uint64_t v;
} __attribute__((packed));

static uint64_t walk(const struct rec *a, size_t n);
static uint64_t pairs(const uint64_t *words, size_t n);
static uint64_t fold_words(const uint64_t *words, size_t n);
static uint64_t big_frame(uint64_t seed);
uint64_t oemu_k_ldst(uint64_t seed);

static uint64_t walk(const struct rec *a, size_t n) {
  uint64_t acc = 0;

  /* Pointer increment inside the loop body is what makes the compiler pick
   * the post-indexed load form rather than a base+immediate one. Each field
   * exercises a different load width and sign-extension. */
  for (; n > 0u; a++, n--) {
    acc ^= (uint64_t)a->big;
    acc += (uint64_t)(int64_t)a->mid;   /* LDRSW/SXTW path */
    acc ^= (uint64_t)(int64_t)a->small; /* sign-extending halfword load */
    acc += (uint64_t)(uint8_t)a->tiny;  /* zero-extending byte load */
  }
  return acc;
}

static uint64_t pairs(const uint64_t *words, size_t n) {
  uint64_t acc = 0;

  /* Adjacent word loads: the compiler folds these into LDP where it can. */
  for (size_t i = 0u; i + 1u < n; i += 2u) {
    acc = (acc + words[i]) ^ words[i + 1u];
  }
  return acc;
}

/* Separate non-inlinable-ish worker so big_frame() really has to save
 * callee-saved registers -- the STP/LDP prologue-epilogue pair. */
static uint64_t fold_words(const uint64_t *words, size_t n) {
  uint64_t acc = 0;

  for (size_t i = 0u; i < n; i++) {
    acc = (acc << 7u) | (acc >> 57u);
    acc += words[i];
  }
  return acc;
}

static uint64_t big_frame(uint64_t seed) {
  /* 128 words of locals: every access is SP-relative, and calling
   * fold_words() forces a pair save/restore around it. */
  uint64_t buf[128];
  uint64_t v = seed | 1u;

  for (size_t i = 0u; i < 128u; i++) {
    v = v * 6364136223846793005ull + 1442695040888963407ull;
    buf[i] = v;
  }
  return fold_words(buf, 128u);
}

uint64_t oemu_k_ldst(uint64_t seed) {
  struct rec arr[16];
  uint64_t words[16];
  uint64_t v = seed;

  for (size_t i = 0u; i < 16u; i++) {
    v = v * 2862933555777941757ull + 3037000493ull;
    arr[i].big = (int64_t)v;
    arr[i].mid = (int32_t)(v >> 40u);
    arr[i].small = (int16_t)(v >> 20u);
    arr[i].tiny = (int8_t)(v >> 8u);
    words[i] = v ^ (uint64_t)(int64_t)arr[i].mid;
  }

  v = walk(arr, 16u) ^ pairs(words, 16u) ^ big_frame(seed);

  /* Unaligned scalar loads: deliberately at an odd byte offset, which the
   * architecture allows and the decoder has to encode identically. */
  {
    const struct unaligned *u = (const struct unaligned *)((const uint8_t *)words + 1u);
    v ^= u->v;
  }
  return v;
}
