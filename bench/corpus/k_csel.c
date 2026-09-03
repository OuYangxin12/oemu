/*
 * Corpus kernel: conditional select and conditional compare.
 *
 * Targets OEMU_OP_CSEL / CSINC / CSINV / CSNEG (ternaries, abs, min/max) and
 * OEMU_OP_CCMP / CCMN (the compiler emits those when a &&-chain or ||-chain
 * both tests flags and feeds a conditional move, so the chains here are
 * written to prefer that lowering).
 */
#include "corpus_common.h"

static int64_t abs64(int64_t x);
static uint64_t clamp_mix(uint64_t a, uint64_t b);
uint64_t oemu_k_csel(uint64_t seed);

static int64_t abs64(int64_t x) {
  /* The canonical CSNEG/CSEL absolute-value idiom. */
  return (x < 0) ? -x : x;
}

static uint64_t clamp_mix(uint64_t a, uint64_t b) {
  /* min/max/ternary cluster: pure CSEL territory, no branches at all. */
  uint64_t lo = (a < b) ? a : b;
  uint64_t hi = (a > b) ? a : b;

  return (lo ^ hi) + (a > 1000u ? b + 1u : b - 1u); /* CSINC / CSNEG shapes */
}

uint64_t oemu_k_csel(uint64_t seed) {
  int64_t v = (int64_t)seed;

  for (unsigned i = 0u; i < 64u; i++) {
    int64_t w = v - (int64_t)i;
    int64_t x = v + (int64_t)i * 3;

    v = (abs64(w) > abs64(x)) ? w : x; /* CSEL on an ABS comparison */
    v = abs64(v);
    v = ((int64_t)(uint64_t)i & 1) != 0 ? v + 1 : v - 1; /* CSINC / CSNEG */

    /* Two condition chains the compiler can only express by merging flags
     * before the conditional move -- the CCMP/CCMN lowering. */
    if (v > 1000 && x > -1000) {
      v ^= (int64_t)0x5555555555555555ull;
    } else if (w < 0 || x > 0) {
      v = (int64_t)((uint64_t)v ^ 3u);
    }

    v = (int64_t)clamp_mix((uint64_t)v, (uint64_t)x);
  }
  return (uint64_t)v;
}
