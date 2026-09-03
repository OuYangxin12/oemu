/*
 * The guest's entire operating system interface: three SVC wrappers.
 *
 * oemu's declared scope is EL0 with SVC-based syscall entry, so a guest
 * benchmark must reach the host exactly this way -- no libc, no stdio, no
 * errno. Numbers are the Linux/AArch64 syscall table; a future oemu
 * syscall layer is expected to implement at least write, clock_gettime and
 * exit_group, which is precisely this trio.
 */
#ifndef OEMU_BENCH_GUEST_SYSCALLS_H
#define OEMU_BENCH_GUEST_SYSCALLS_H

#include <stddef.h>
#include <stdint.h>

/* struct timespec as the kernel sees it on AArch64: two 64-bit fields. */
struct guest_timespec {
  int64_t tv_sec;
  int64_t tv_nsec;
};

#define GUEST_CLOCK_REALTIME  0
#define GUEST_CLOCK_MONOTONIC 1

/* Returns the kernel's return value (negative errno encoding on failure). */
long guest_write(int fd, const void *buf, size_t len);
long guest_clock_gettime(int clock_id, struct guest_timespec *ts);

/* Never returns; takes the low 8 bits as the exit status. */
void guest_exit(int code) __attribute__((noreturn));

#endif /* OEMU_BENCH_GUEST_SYSCALLS_H */
