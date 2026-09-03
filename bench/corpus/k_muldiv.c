/*
 * Corpus kernel: multiply and divide.
 *
 * Targets OEMU_OP_MADD / MSUB, OEMU_OP_SMADDL / UMADDL and the SMULH / UMULH
 * high halves (the 128-bit builtin is what lowers to them), plus OEMU_OP_UDIV
 * / SDIV through a runtime divisor. Divisors are forced odd so the corpus
 * never contains a division by zero. Constant divisors additionally pull in
 * the reciprocal-multiply lowering, which is MADD/MSUB-dense.
 */
#include "corpus_common.h"

static uint64_t mulwide(uint64_t a, uint64_t b);
uint64_t oemu_k_muldiv(uint64_t seed, uint64_t divisor);

static uint64_t mulwide(uint64_t a, uint64_t b) {
  /* __int128 is the standard idiom for the 64x64->128 forms; both the low
   * and the high half of the product are read, so MUL and UMULH/SMULH both
   * appear. */
  unsigned __int128 p = (unsigned __int128)a * (unsigned __int128)b;

  return (uint64_t)p ^ (uint64_t)(p >> 64u);
}

uint64_t oemu_k_muldiv(uint64_t seed, uint64_t divisor) {
  uint64_t v = seed | 1u;
  uint64_t d = divisor | 1u;

  for (unsigned i = 0u; i < 64u; i++) {
    uint64_t m = d + (uint64_t)i;

    v = mulwide(v, m) + (v / d) + (v % d);                        /* UDIV and MUL/MADD */
    v ^= (uint64_t)((int64_t)v / (int64_t)d);                     /* SDIV path */
    v = v * v - (v / 3u) * 3u - (v / 7u);                         /* reciprocal multiply */
    v = (uint64_t)((int64_t)(v >> 1u) * (int64_t)0x300000000ull); /* SMADDL shape */
    if (v == d) {
      v = seed ^ (uint64_t)i; /* stay off the degenerate fixed point */
    }
  }
  return v;
}
