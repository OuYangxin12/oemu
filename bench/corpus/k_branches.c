/*
 * Corpus kernel: every branch shape.
 *
 * Targets OEMU_OP_B / BL / B_COND / CBZ / CBNZ / TBZ / TBNZ / BR / BLR. The
 * sparse sixteen-arm switch makes the compiler emit either a comparison
 * chain or a jump table (both worth having in the corpus); the function
 * pointer array is the indirect-BLR shape an executor's dispatch loop and
 * every C++-style vtable call share; the single-bit tests are the
 * TBZ/TBNZ lowering.
 */
#include "corpus_common.h"

static uint64_t arm_a(uint64_t v);
static uint64_t arm_b(uint64_t v);
static uint64_t arm_c(uint64_t v);
static uint64_t arm_d(uint64_t v);

static uint64_t (*const arms[4])(uint64_t);

uint64_t oemu_k_branches(uint64_t seed);

static uint64_t arm_a(uint64_t v) {
  return v + 0x9e3779b9u;
}
static uint64_t arm_b(uint64_t v) {
  return v ^ (v >> 27u);
}
static uint64_t arm_c(uint64_t v) {
  return v - (v << 2u);
}
static uint64_t arm_d(uint64_t v) {
  return (v << 5u) | (v >> 59u);
}

static uint64_t (*const arms[4])(uint64_t) = {arm_a, arm_b, arm_c, arm_d};

uint64_t oemu_k_branches(uint64_t seed) {
  uint64_t v = seed;
  uint64_t n = 64u;

  while (n != 0u) { /* the loop-end test itself lowers to CBZ/CBNZ shapes */
    switch ((v >> 40u) & 0xfu) {
      case 0u:
        v = arm_a(v);
        break;
      case 1u:
        v = arm_b(v) + 1u;
        break;
      case 2u:
        v = arm_c(v) ^ 0x0ff00ff0u;
        break;
      case 3u:
        v = arm_d(v) + arm_a(v);
        break;
      case 4u:
        v = arm_a(v) - arm_b(v);
        break;
      case 5u:
        v = arm_b(v) * 3u;
        break;
      case 6u:
        v = arm_c(v) | arm_d(v);
        break;
      case 7u:
        v = arm_d(v) & 0x7fff0000ffffu;
        break;
      case 8u:
        v = arm_a(v) ^ arm_c(v);
        break;
      case 9u:
        v = arm_b(v) + arm_d(v);
        break;
      case 10u:
        v = arm_c(v) - 0x9e3779b9u;
        break;
      case 11u:
        v = arm_d(v) | (v >> 33u);
        break;
      case 12u:
        v = arm_a(v) * 5u;
        break;
      case 13u:
        v = arm_b(v) - (v & 0xf00fu);
        break;
      case 14u:
        v = arm_c(v) ^ 0xdeadu;
        break;
      case 15u:
      default:
        v = arm_d(v) + 0x1000000000000ull;
        break;
    }

    /* Single-bit test on the low 32 bits: the TBZ/TBNZ lowering. */
    if ((v & (1ull << (n & 31u))) != 0u) {
      v = arms[n & 3u](v); /* BLR through the table */
    } else {
      v = arms[(n + 1u) & 3u](v ^ n);
    }

    /* do-while shape: a tail branch that always tests, not just on wrap. */
    do {
      v = arm_a(v ^ (v << 1u));
      n = n - 1u;
    } while ((n & 3u) != 0u);
  }
  return v;
}
