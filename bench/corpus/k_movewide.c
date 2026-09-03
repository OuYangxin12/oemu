/*
 * Corpus kernel: wide immediates and PC-relative addressing.
 *
 * Targets OEMU_OP_MOVZ / MOVN / MOVK (the odd 64-bit constants force
 * multi-MOVK chains) and OEMU_OP_ADR / ADRP (taking addresses of static data
 * and of functions lowers to the two PC-relative forms under the small code
 * model).
 */
#include "corpus_common.h"

/* All eight are hand-picked so no single MOVZ suffices: several need three
 * MOVK patches, and the ones with many low zeros favour MOVN. */
static const uint64_t wide_constants[8] = {
    0x9e3779b97f4a7c15ull, 0xbf58476d1ce4e5b9ull, 0x94d049bb133111ebull, 0x8000000000000000ull,
    0x0000ffffe0000000ull, 0xdeadbeef12345678ull, 0x000000fff000000full, 0xffff0000ffff0000ull};

static uint64_t fold(uint64_t v, uint64_t k);
uint64_t oemu_k_movewide(uint64_t seed);

static uint64_t fold(uint64_t v, uint64_t k) {
  const uint64_t c0 = 0x9e3779b97f4a7c15ull; /* full-precision constants: the
                                                MOVZ/MOVN/MOVK point of this
                                                file */
  const uint64_t c1 = 0xbf58476d1ce4e5b9ull;
  const uint64_t c2 = 0x00000000ffff0000ull;

  v = (v ^ c0) + k * c1;
  v ^= (v >> 31u) ^ (c2 - k);
  if ((v & 0x8000000000000000ull) != 0u) {
    v -= 0x8000000000000000ull;
  }
  return v;
}

uint64_t oemu_k_movewide(uint64_t seed) {
  /* Address of the table: ADRP plus an addressing-mode fold in the compiler. */
  const uint64_t *table = wide_constants;
  uint64_t v = seed;

  for (unsigned i = 0; i < 32u; i++) {
    v = fold(v, table[i & 7u]);
    /* Taking the table address again on every iteration keeps the ADRP form
     * live inside the loop body, where an executor sees it most. */
    v ^= table[i & 7u] + (uint64_t)(uintptr_t)table;
  }
  return v;
}
