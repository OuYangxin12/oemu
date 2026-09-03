/*
 * Freestanding C runtime for guest benchmarks.
 *
 * Two jobs:
 *   - _start: the ELF entry point. The kernel (or oemu, later) hands over a
 *     16-byte-aligned stack in SP and nothing else; we run the benchmark and
 *     translate its integer result into an exit_group status.
 *   - memcpy / memmove / memset: at -O2 the compiler turns eligible loops
 *     and structure assignments into implicit calls to exactly these three,
 *     and there is no libc to provide them. Freestanding builds must supply
 *     them; these are deliberately simple byte/word loops, not tuned.
 */
#include <stddef.h>
#include <stdint.h>

#include "guest_syscalls.h"

int benchmark_main(void); /* provided by the benchmark translation unit */

void _start(void);
void *memcpy(void *dest, const void *src, size_t num);
void *memmove(void *dest, const void *src, size_t num);
void *memset(void *dest, int value, size_t num);

void _start(void) {
  int status = benchmark_main();
  guest_exit(status);
}

void *memcpy(void *dest, const void *src, size_t num) {
  uint8_t *d = (uint8_t *)dest;
  const uint8_t *s = (const uint8_t *)src;

  for (size_t i = 0; i < num; i++) {
    d[i] = s[i];
  }
  return dest;
}

void *memmove(void *dest, const void *src, size_t num) {
  uint8_t *d = (uint8_t *)dest;
  const uint8_t *s = (const uint8_t *)src;

  if (d == s || num == 0u || d + num <= s || s + num <= d) {
    return memcpy(dest, src, num);
  }
  if (d < s) {
    for (size_t i = 0; i < num; i++) {
      d[i] = s[i];
    }
  } else {
    for (size_t i = num; i > 0u; i--) {
      d[i - 1u] = s[i - 1u];
    }
  }
  return dest;
}

void *memset(void *dest, int value, size_t num) {
  uint8_t *d = (uint8_t *)dest;

  for (size_t i = 0; i < num; i++) {
    d[i] = (uint8_t)value;
  }
  return dest;
}
