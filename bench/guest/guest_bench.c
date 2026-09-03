/*
 * oemu guest benchmark: intmix.
 *
 * A deterministic integer stress for the end-to-end phase: arithmetic,
 * CRC loops, call overhead and table lookups, sized so an emulator run is
 * milliseconds in native terms and seconds under interpretation.
 *
 * Two properties are deliberate and matter once oemu executes this:
 *   - Output is fully deterministic (no timing, no addresses). A future
 *     `oemu run` harness can byte-compare stdout to detect regressions.
 *   - Every result is self-checked by redundant computation: two
 *     independent implementations of the same function must agree, so a
 *     mis-emulated instruction fails the run rather than skewing a number.
 *
 * Compile with -DGUEST_TIMING to append an elapsed-time line; that variant
 * is not byte-deterministic and is the one used for cycle counting.
 */
#include <stddef.h>
#include <stdint.h>

#include "guest_syscalls.h"

#define ITERS    4096u
#define BUF_SIZE 512u

struct node {
  uint64_t val;
  int32_t next;
};

static uint8_t buffer[BUF_SIZE];
static struct node chain[64];
static uint8_t crc_table[256];

static void put_str(const char *s);
static void put_u64_dec(uint64_t v);
static void put_u64_hex(uint64_t v);
static void crc_build_table(void);
static uint8_t crc8_fast(const uint8_t *bytes, size_t n);
static uint8_t crc8_slow(const uint8_t *bytes, size_t n);
static uint64_t fnv1a_bytes(const uint8_t *bytes, size_t n);
static uint64_t fnv1a_words(const uint8_t *bytes, size_t n);
static void seed_buffers(uint64_t seed);
static uint64_t chain_sum(void);

static void put_str(const char *s) {
  size_t n = 0u;

  while (s[n] != '\0') {
    n++;
  }
  (void)guest_write(1, s, n);
}

static void put_u64_dec(uint64_t v) {
  char tmp[20];
  size_t i = sizeof(tmp);

  do {
    i--;
    tmp[i] = (char)('0' + (int)(v % 10u));
    v /= 10u;
  } while (v != 0u);
  (void)guest_write(1, &tmp[i], sizeof(tmp) - i);
}

static void put_u64_hex(uint64_t v) {
  static const char digits[] = "0123456789abcdef";
  char tmp[16];

  for (size_t i = 0; i < 16u; i++) {
    tmp[15u - i] = digits[(size_t)(v & 0xfu)];
    v >>= 4u;
  }
  (void)guest_write(1, tmp, sizeof(tmp));
}

static void crc_build_table(void) {
  for (unsigned i = 0u; i < 256u; i++) {
    uint8_t c = (uint8_t)i;

    for (int b = 0; b < 8; b++) {
      c = (uint8_t)((c >> 1) ^ (uint8_t)(0x07 & (uint8_t)(-(int8_t)(c & 1u))));
    }
    crc_table[i] = c;
  }
}

static uint8_t crc8_fast(const uint8_t *bytes, size_t n) {
  uint8_t c = 0u;

  for (size_t i = 0u; i < n; i++) {
    c = crc_table[(uint8_t)(c ^ bytes[i])];
  }
  return c;
}

static uint8_t crc8_slow(const uint8_t *bytes, size_t n) {
  uint8_t c = 0u;

  for (size_t i = 0u; i < n; i++) {
    c ^= bytes[i];
    for (int b = 0; b < 8; b++) {
      c = (uint8_t)((c >> 1) ^ (uint8_t)(0x07 & (uint8_t)(-(int8_t)(c & 1u))));
    }
  }
  return c;
}

/* Two FNV-1a implementations over the same buffer: byte at a time, and
 * eight at a time (one multiply per word instead of eight). Same function,
 * different instruction mix -- agreement is the check. */
static uint64_t fnv1a_bytes(const uint8_t *bytes, size_t n) {
  uint64_t h = 1469598103934665603ull;

  for (size_t i = 0u; i < n; i++) {
    h ^= (uint64_t)bytes[i];
    h *= 1099511628211ull;
  }
  return h;
}

static uint64_t fnv1a_words(const uint8_t *bytes, size_t n) {
  uint64_t h = 1469598103934665603ull;

  for (size_t i = 0u; i < n; i += 8u) {
    uint64_t w = (uint64_t)bytes[i];

    w |= (uint64_t)bytes[i + 1u] << 8u;
    w |= (uint64_t)bytes[i + 2u] << 16u;
    w |= (uint64_t)bytes[i + 3u] << 24u;
    w |= (uint64_t)bytes[i + 4u] << 32u;
    w |= (uint64_t)bytes[i + 5u] << 40u;
    w |= (uint64_t)bytes[i + 6u] << 48u;
    w |= (uint64_t)bytes[i + 7u] << 56u;

    h ^= w & 0xffu;
    h *= 1099511628211ull;
    h ^= (w >> 8u) & 0xffu;
    h *= 1099511628211ull;
    h ^= (w >> 16u) & 0xffu;
    h *= 1099511628211ull;
    h ^= (w >> 24u) & 0xffu;
    h *= 1099511628211ull;
    h ^= (w >> 32u) & 0xffu;
    h *= 1099511628211ull;
    h ^= (w >> 40u) & 0xffu;
    h *= 1099511628211ull;
    h ^= (w >> 48u) & 0xffu;
    h *= 1099511628211ull;
    h ^= (w >> 56u) & 0xffu;
    h *= 1099511628211ull;
  }
  return h;
}

static void seed_buffers(uint64_t seed) {
  uint64_t s = seed | 1u;

  for (size_t i = 0u; i < BUF_SIZE; i++) {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    buffer[i] = (uint8_t)(s >> 33u);
  }
  for (unsigned i = 0u; i < 64u; i++) {
    chain[i].val = s + (uint64_t)i * 0x9e3779b97f4a7c15ull;
    chain[i].next = (int32_t)((i + 1u) % 64u);
  }
}

static uint64_t chain_sum(void) {
  uint64_t acc = 0;
  int32_t at = 0;

  /* Pointer-chasing through the chain: the list-walk shape Dhrystone and
   * CoreMark both rate on, as a counted loop so it terminates regardless. */
  for (unsigned step = 0u; step < 64u; step++) {
    acc ^= chain[at].val + (uint64_t)step * 0x100000001b3ull;
    at = chain[at].next;
  }
  return acc;
}

int benchmark_main(void) {
  uint64_t checksum = 0;
  int failures = 0;
#ifdef GUEST_TIMING
  struct guest_timespec t0;
  struct guest_timespec t1;

  (void)guest_clock_gettime(GUEST_CLOCK_MONOTONIC, &t0);
#endif

  crc_build_table();
  seed_buffers(0x2545f4914f6cdd1dull);

  for (unsigned iter = 0u; iter < ITERS; iter++) {
    uint8_t fast = crc8_fast(buffer, BUF_SIZE);
    uint8_t slow = crc8_slow(buffer, BUF_SIZE);
    uint64_t fa = fnv1a_bytes(buffer, BUF_SIZE);
    uint64_t fw = fnv1a_words(buffer, BUF_SIZE);

    if (fast != slow) {
      failures++;
    }
    if (fa != fw) {
      failures++;
    }
    /* Rotate the buffer right by one so later iterations differ, keeping the
     * redundant paths honest on the new contents too. Copying high-to-low
     * with an explicit carry is the in-place rotation; the naive loop would
     * read bytes it has already overwritten. */
    {
      uint8_t carry = buffer[BUF_SIZE - 1u];

      for (size_t i = BUF_SIZE; i > 1u; i--) {
        buffer[i - 1u] = buffer[i - 2u];
      }
      buffer[0] = carry;
    }
    checksum = (checksum << 7u) | (checksum >> 57u);
    checksum ^= (uint64_t)fast + (uint64_t)fa + chain_sum();

    /* Perturb the seed path so the deterministic stream advances. */
    for (size_t i = 0u; i < BUF_SIZE; i += 8u) {
      buffer[i] = (uint8_t)(buffer[i] ^ (uint8_t)iter);
    }
  }

#ifdef GUEST_TIMING
  (void)guest_clock_gettime(GUEST_CLOCK_MONOTONIC, &t1);
  put_str("oemu-guest: intmix elapsed_ns=");
  put_u64_dec(((uint64_t)(int64_t)(t1.tv_sec - t0.tv_sec) * 1000000000ull) +
              (uint64_t)(int64_t)(t1.tv_nsec - t0.tv_nsec));
  put_str("\n");
#endif

  put_str("oemu-guest: intmix iters=");
  put_u64_dec((uint64_t)ITERS);
  put_str(" failures=");
  put_u64_dec((uint64_t)failures);
  put_str(" checksum=0x");
  put_u64_hex(checksum);
  put_str("\n");

  return (failures == 0) ? 0 : 1;
}
