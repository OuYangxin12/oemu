/*
 * SVC implementations for the guest benchmark runtime.
 *
 * Written with the register variables and explicit asm the Linux/AArch64
 * ABI requires (syscall number in x8, arguments x0..x5, return in x0). This
 * is the same shape guest code will use under oemu's future syscall layer,
 * which makes the generated encodings as interesting as the functionality.
 */
#include "guest_syscalls.h"

#define GUEST_SYS_clock_gettime 113
#define GUEST_SYS_write         64
#define GUEST_SYS_exit_group    94

long guest_write(int fd, const void *buf, size_t len) {
  register long x0 __asm__("x0") = (long)fd;
  register long x1 __asm__("x1") = (long)(uintptr_t)buf;
  register long x2 __asm__("x2") = (long)len;
  register long x8 __asm__("x8") = GUEST_SYS_write;

  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x2), "r"(x8) : "memory");
  return x0;
}

long guest_clock_gettime(int clock_id, struct guest_timespec *ts) {
  register long x0 __asm__("x0") = (long)clock_id;
  register long x1 __asm__("x1") = (long)(uintptr_t)ts;
  register long x8 __asm__("x8") = GUEST_SYS_clock_gettime;

  __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
  return x0;
}

void guest_exit(int code) {
  register long x0 __asm__("x0") = (long)(code & 0xff);
  register long x8 __asm__("x8") = GUEST_SYS_exit_group;

  __asm__ volatile("svc #0" : : "r"(x0), "r"(x8) : "memory");
  __builtin_unreachable();
}
