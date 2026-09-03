/*
 * Corpus kernel: a realistic hash/checksum mix.
 *
 * Not tied to one opcode family: CRC bit loops, an LCG, xorshift64* and
 * FNV-1a are the combination that shows up in real compiled integer code,
 * and the densest instruction mixing per byte of source is what the decode
 * throughput number is most sensitive to.
 */
#include "corpus_common.h"

static uint8_t crc8_step(uint8_t c, uint8_t data);
static uint64_t fnv1a(const uint8_t *bytes, size_t n);
static uint64_t xorshift64star(uint64_t *state);
uint64_t oemu_k_hash(uint64_t seed, const uint8_t *bytes, size_t n);

static uint8_t crc8_step(uint8_t c, uint8_t data) {
  c ^= data;
  /* The shift-and-xor-conditional inner loop: the (c >> 1) ^ (poly & -(c & 1))
   * form keeps the inner loop branch-free, which is what real CRC code does
   * and what the flags/logic decode paths are exercised by. */
  for (int b = 0; b < 8; b++) {
    c = (uint8_t)((c >> 1) ^ (uint8_t)(0x07 & (uint8_t)(-(int8_t)(c & 1u))));
  }
  return c;
}

static uint64_t fnv1a(const uint8_t *bytes, size_t n) {
  uint64_t h = 1469598103934665603ull;

  for (size_t i = 0u; i < n; i++) {
    h ^= (uint64_t)bytes[i];
    h *= 1099511628211ull;
  }
  return h;
}

static uint64_t xorshift64star(uint64_t *state) {
  uint64_t x = *state;

  x ^= x >> 12u;
  x ^= x << 25u;
  x ^= x >> 27u;
  *state = x;
  return x * 2685821657736338717ull;
}

uint64_t oemu_k_hash(uint64_t seed, const uint8_t *bytes, size_t n) {
  uint64_t state = seed | 1u;
  uint64_t lcg = seed ^ 0x853c49e6748fea9bull;
  uint8_t crc = 0u;
  uint64_t mix = 0u;

  for (size_t i = 0u; i < n; i++) {
    crc = crc8_step(crc, bytes[i]);
    lcg = lcg * 6364136223846793005ull + 1442695040888963407ull;
    mix ^= (uint64_t)crc + (lcg >> 33u) + (uint64_t)bytes[i] * 31u;
    if ((i & 7u) == 0u) {
      mix ^= xorshift64star(&state);
    }
  }
  return mix ^ fnv1a(bytes, n) ^ lcg ^ (uint64_t)crc;
}
