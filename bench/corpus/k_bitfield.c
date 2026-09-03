/*
 * Corpus kernel: bitfield moves, one-source ops and variable shifts.
 *
 * Targets OEMU_OP_SBFM / BFM / UBFM / EXTR (extract/insert and the rotate
 * idiom lower to EXTR), OEMU_OP_CLZ / CLS (the compiler builtins), and
 * OEMU_OP_RBIT / REV16 / REV32 / REV (byte-swap builtins). The variable
 * shift through the 6-bit mask is the LSLV/LSRV/ASRV/RORV shape.
 */
#include "corpus_common.h"

static uint64_t ror64(uint64_t v, unsigned k);
static int32_t extract_signed(int64_t v, unsigned lo, unsigned width);
static uint64_t pack4(uint8_t a, uint8_t b, uint8_t c, uint8_t d);
uint64_t oemu_k_bitfield(uint64_t seed);

static uint64_t ror64(uint64_t v, unsigned k) {
  /* The (x << k) | (x >> (64 - k)) idiom lowers to EXTR -- the exact encoding
   * the decoder must distinguish from two independent shifts. The &63 keeps
   * the shift in range so k = 0 stays defined and still yields v. */
  k &= 63u;
  return (v << k) | (v >> ((64u - k) & 63u));
}

static int32_t extract_signed(int64_t v, unsigned lo, unsigned width) {
  /* shift-left-then-arithmetic-shift-right: the canonical SBFM lowering. */
  return (int32_t)((v << (64u - lo - width)) >> (64u - width));
}

static uint64_t pack4(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
  return (uint64_t)a | ((uint64_t)b << 8u) | ((uint64_t)c << 16u) | ((uint64_t)d << 24u);
}

uint64_t oemu_k_bitfield(uint64_t seed) {
  uint64_t v = seed | 1u;

  for (unsigned i = 1u; i < 64u; i++) {
    v = ror64(v, i);
    v ^= (uint64_t)__builtin_clzll((unsigned long long)v);
    /* cls(x) == clz(x ^ (x >> 63)) - 1: the standard lowering, and the form
     * that reaches the CLZ/CLS decode paths on compilers without a CLS
     * builtin (clang has none). */
    v += (uint64_t)(int64_t)(__builtin_clzll(
                                 (unsigned long long)(v ^ (uint64_t)((int64_t)v >> 63))) -
                             1);
    v = __builtin_bswap64(v);                                      /* REV */
    v ^= (uint64_t)(uint32_t)__builtin_bswap32((unsigned int)v);   /* REV32 */
    v ^= (uint64_t)(uint32_t)__builtin_bswap16((unsigned short)v); /* REV16 */
    v = ror64(v, (unsigned)v & 63u);
    v = pack4((uint8_t)v, (uint8_t)(v >> 8u), (uint8_t)(v >> 16u), (uint8_t)(v >> 24u)) +
        (uint64_t)(uint32_t)extract_signed((int64_t)v, 8u, 12u);
    v ^= (v << (i & 63u)) | (v >> (i & 63u)); /* variable shifts, RORV shape */
  }
  return v;
}
