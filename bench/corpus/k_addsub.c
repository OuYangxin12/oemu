/*
 * Corpus kernel: the add/subtract families.
 *
 * Targets OEMU_OP_ADD / ADDS / SUB / SUBS / ADC / ADCS / SBC / SBCS in their
 * immediate, shifted-register and extended-register operand forms. The
 * carry-propagating 128-bit addition is what forces ADC/ADCS and SBC/SBCS
 * codegen; the narrowing casts are what make the compiler emit the
 * extended-register forms (UXTB/UXTH/SXTW/SXTX with and without a shift).
 */
#include "corpus_common.h"

struct pair128 {
  uint64_t lo;
  uint64_t hi;
};

static struct pair128 add128(struct pair128 x, struct pair128 y);
static uint64_t add_extended(uint32_t a, uint16_t b, uint8_t c);
static uint64_t add_shifted(uint64_t a, uint64_t b);
static uint64_t sub_shifted(uint64_t a, uint64_t b);
uint64_t oemu_k_addsub(uint64_t seed);

static struct pair128 add128(struct pair128 x, struct pair128 y) {
  struct pair128 r;

  /* Adds set C on wrap; the carry-out then feeds the high half through the
   * ADC form, which is exactly the two-word pattern the flags code must get
   * right. */
  r.lo = x.lo + y.lo;
  r.hi = x.hi + y.hi + (uint64_t)(r.lo < x.lo);
  return r;
}

static uint64_t add_extended(uint32_t a, uint16_t b, uint8_t c) {
  /* One of each extension, two of them scaled by a shift -- all three of the
   * option/imm3 field combinations the decoder has to expand. */
  return (uint64_t)(int64_t)(int32_t)a + (uint64_t)(int64_t)(int16_t)b + (uint64_t)c * 4096u +
         (uint64_t)c;
}

static uint64_t add_shifted(uint64_t a, uint64_t b) {
  return a + (b << 4u) + (b >> 12u) + (a + b * 4096u) + (a + b * 16u);
}

static uint64_t sub_shifted(uint64_t a, uint64_t b) {
  uint64_t x = a - (b << 3u);
  uint64_t y = b - (a >> 7u);

  /* Subs/Adcs/Sbcs chain: borrow out of the first word becomes the carry-in
   * of the second, so the flag semantics decide the result. */
  return (x - y) + (uint64_t)((x < y) ? -1 : 0) + (x ^ y);
}

uint64_t oemu_k_addsub(uint64_t seed) {
  uint64_t acc = seed;

  for (unsigned i = 0; i < 64u; i++) {
    struct pair128 p;
    struct pair128 q;

    acc = add_shifted(acc, (uint64_t)i + 4096u);
    acc = add_extended((uint32_t)acc, (uint16_t)(acc >> 32u), (uint8_t)i);
    acc = sub_shifted(acc, (uint64_t)i * 8u + 1u);

    p.lo = acc;
    p.hi = (uint64_t)(int64_t) - (int64_t)acc;
    q.lo = (uint64_t)i ^ 0x1000000010000000ull;
    q.hi = acc >> 63u;
    acc = add128(p, q).lo;
  }
  return acc;
}
