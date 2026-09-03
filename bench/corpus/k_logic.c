/*
 * Corpus kernel: the logical families.
 *
 * Targets OEMU_OP_AND / ANDS / ORR / ORN / EOR / EON / BIC / BICS and their
 * shifted-register variants. The immediates here are chosen to stress the
 * NMT/immr/imms bitmask expansion in every shape it supports: alternating
 * masks, rotated runs of ones, and single-bit tests (TST, which is ANDS XZR).
 */
#include "corpus_common.h"

static uint64_t mask_round(uint64_t v);
static uint64_t shifted_logic(uint64_t a, uint64_t b);
uint64_t oemu_k_logic(uint64_t seed, uint64_t key);

static uint64_t mask_round(uint64_t v) {
  /* Four different legal bitmask immediates, one per supported pattern:
   * repeated byte pairs, nibble runs, a single wide run crossing lane
   * boundaries, and the top-bit test pattern. */
  uint64_t t = v & 0xff00ff00ff00ff00ull;
  t |= (v ^ 0x00ff00ff00ff00ffull);
  t ^= 0x0000ffff0000ffffull;
  if ((t & 0x8000000000000000ull) != 0u) {
    t |= 0xf0f0f0f0f0f0f0f0ull;
  } else {
    t &= ~0xf0f0f0f0f0f0f0f0ull; /* BIC with an expanded immediate */
  }
  return t;
}

static uint64_t shifted_logic(uint64_t a, uint64_t b) {
  uint64_t t = a & (b >> 8u); /* AND LSR */
  t |= (a << 3u) ^ b;         /* EOR / ORR with LSL */
  t ^= ~(a >> 16u);           /* EON with an inverted shifted operand */
  return t;
}

uint64_t oemu_k_logic(uint64_t seed, uint64_t key) {
  uint64_t v = seed;

  for (unsigned i = 0; i < 96u; i++) {
    uint64_t k = key + (uint64_t)i * 0x9e3779b9u;

    v = mask_round(v) ^ k;
    v = shifted_logic(v, k);
    /* The ANDS-with-zero idiom (TST) guarding a branch: flags from a logical
     * op decide control flow, the same as in real compiled code. */
    if ((v | (v >> 32u)) == 0u) {
      v = k;
    }
  }
  return v;
}
