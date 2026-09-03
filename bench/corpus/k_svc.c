/*
 * Corpus kernel: system-instruction territory.
 *
 * Targets OEMU_OP_SVC -- the single instruction the executor must route to
 * the syscall layer rather than interpret -- plus OEMU_OP_NOP and the hint
 * encodings. The blob is never executed, so a write(2) to a closed fd is
 * safe here: the *encoding* is what the corpus wants, and this is the one
 * file that can produce the real instruction sequence guest code will use.
 */
#include "corpus_common.h"

static long guest_svc3(long number, long a0, long a1, long a2);
uint64_t oemu_k_svc(uint64_t seed, long fd, const char *text, long len);

static long guest_svc3(long number, long a0, long a1, long a2) {
  /* The exact register discipline Linux/AArch64 demands (syscall number in
   * x8, arguments in x0..x5), written the way a guest runtime writes it:
   * MOVZ/MOV for x8, the argument moves, then SVC #0. */
  register long x0 __asm__("x0") = a0;
  register long x1 __asm__("x1") = a1;
  register long x2 __asm__("x2") = a2;
  register long x8 __asm__("x8") = number;

  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}

uint64_t oemu_k_svc(uint64_t seed, long fd, const char *text, long len) {
  uint64_t acc = seed;

  /* write(2): fd, buffer, length -- the shape every guest output path uses. */
  {
    long r = guest_svc3(64, fd, (long)(uintptr_t)text, len);

    acc ^= (uint64_t)(int64_t)r;
  }
  /* Hints must survive the decode as no-ops, so keep some in the stream. */
  __asm__ volatile("nop");
  __asm__ volatile("yield");

  return acc + (uint64_t)len;
}
