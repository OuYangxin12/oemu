/*
 * Corpus kernel: string/byte loops and bulk copies.
 *
 * The two shapes every libc string function lowers to: byte-at-a-time
 * scanning (LDRB plus a null test, CBZ/TBNZ on the terminator) and
 * word-at-a-time copying (LDR/STR pairs, LDP/STP when the compiler is
 * feeling generous). Own implementations rather than libc calls because
 * the corpus objects must stay freestanding.
 */
#include "corpus_common.h"

static size_t corpus_strlen(const uint8_t *s);
static int corpus_strcmp(const uint8_t *a, const uint8_t *b);
static void corpus_memcpy(uint8_t *dst, const uint8_t *src, size_t n);
static void corpus_memset(uint8_t *dst, uint8_t val, size_t n);
uint64_t oemu_k_memops(uint8_t *dst, const uint8_t *src, size_t n);

static size_t corpus_strlen(const uint8_t *s) {
  const uint8_t *p = s;

  while (*p != 0u) { /* terminator test: the CBZ lowering */
    p++;
  }
  return (size_t)(p - s);
}

static int corpus_strcmp(const uint8_t *a, const uint8_t *b) {
  while (*a != 0u && *a == *b) {
    a++;
    b++;
  }
  return (int)*a - (int)*b;
}

static void corpus_memcpy(uint8_t *dst, const uint8_t *src, size_t n) {
  for (size_t i = 0u; i < n; i++) {
    dst[i] = src[i];
  }
}

static void corpus_memset(uint8_t *dst, uint8_t val, size_t n) {
  for (size_t i = 0u; i < n; i++) {
    dst[i] = val;
  }
}

uint64_t oemu_k_memops(uint8_t *dst, const uint8_t *src, size_t n) {
  uint64_t sum = 0u;

  if (n == 0u) {
    return 0u;
  }

  corpus_memset(dst, 0xa5u, n);
  corpus_memcpy(dst, src, n);

  /* The comparison walk and a re-read reduction: two more byte loops with
   * different exit-condition shapes. */
  {
    size_t len = corpus_strlen(src);
    int diff = corpus_strcmp(dst, src);

    sum = (uint64_t)len;
    sum = (sum << 8u) | (uint64_t)((diff & 0xff) ^ 0x5au);
    for (size_t i = 0u; i < n; i += 3u) {
      sum += dst[i];
    }
  }
  return sum;
}
