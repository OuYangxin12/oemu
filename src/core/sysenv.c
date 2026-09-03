/*
 * Minimal Linux-AArch64 compatible SVC surface. See include/oemu/sysenv.h.
 *
 * Deliberate limitations, all visible from the returned errno:
 *   - write() only accepts fds 1 and 2; there is no file table.
 *   - A buffer must lie wholly inside one mapped, readable region; a guest
 *     buffer split across two regions is answered -EFAULT rather than being
 *     stitched together, because the alternative would hide guest bugs.
 *   - clock_gettime reads the real host clock. Determinism-critical callers
 *     (the executor's own tests) must not read the clock fields for equality.
 */
/*
 * This file uses one POSIX function (clock_gettime), which strict-C11 glibc
 * hides unless _POSIX_C_SOURCE is defined. The macro is supplied by the build
 * (see src/CMakeLists.txt) rather than here so the source carries no reserved
 * _-prefixed macro definition.
 */
#include "oemu/sysenv.h"

#include <time.h>

void oemu_sysenv_init(oemu_sysenv *env, FILE *out) {
  if (env == NULL) {
    return;
  }
  env->out = out;
  env->exit_code = 0;
  env->exited = false;
}

bool oemu_sysenv_exited(const oemu_sysenv *env) {
  return (env != NULL) && env->exited;
}

int oemu_sysenv_exit_code(const oemu_sysenv *env) {
  return (env != NULL && env->exited) ? env->exit_code : 0;
}

static int64_t sys_write(oemu_sysenv *env, oemu_memory *mem, uint64_t fd, uint64_t buf,
                         uint64_t count) {
  if (fd != 1ULL && fd != 2ULL) {
    return -OEMU_EBADF;
  }
  if (count == 0ULL) {
    return 0;
  }
  if (count > (UINT64_MAX - buf)) {
    return -OEMU_EFAULT; /* the range wraps; no region can contain it */
  }
  if (oemu_memory_validate(mem, buf, count, OEMU_PERM_READ) != OEMU_OK) {
    return -OEMU_EFAULT;
  }
  /* Read the bytes out of the guest one at a time into a small bounce buffer:
   * the region API has no raw-host-pointer entry point, and a syscall is
   * nowhere near the hot path where that would matter. */
  FILE *stream = (env->out != NULL) ? env->out : stdout;
  uint8_t chunk[256];
  uint64_t written = 0ULL;
  while (written < count) {
    uint64_t want = count - written;
    if (want > sizeof(chunk)) {
      want = sizeof(chunk);
    }
    for (uint64_t i = 0; i < want; i++) {
      uint64_t byte = 0U;
      if (oemu_memory_read(mem, buf + written + i, OEMU_MEM_BYTE, false, &byte) != OEMU_OK) {
        return (written > 0ULL) ? (int64_t)written : -OEMU_EFAULT;
      }
      chunk[i] = (uint8_t)(byte & UINT64_C(0xFF));
    }
    const size_t got = fwrite(chunk, 1U, (size_t)want, stream);
    written += (uint64_t)got;
    if (got < (size_t)want) {
      break; /* host stream failed; report what made it */
    }
  }
  return (int64_t)written;
}

static int64_t sys_clock_gettime(oemu_memory *mem, uint64_t clock_id, uint64_t ts_gva) {
  struct timespec ts;
  if (clock_id != OEMU_CLOCK_REALTIME && clock_id != OEMU_CLOCK_MONOTONIC) {
    return -OEMU_EINVAL;
  }
  if (clock_gettime((clockid_t)clock_id, &ts) != 0) {
    return -OEMU_EFAULT;
  }
  /* struct timespec in the guest is two 64-bit fields, seconds then
   * nanoseconds; both happen to match the host on AArch64 LP64. */
  if (oemu_memory_validate(mem, ts_gva, 8ULL, OEMU_PERM_WRITE) != OEMU_OK ||
      oemu_memory_validate(mem, ts_gva + 8ULL, 8ULL, OEMU_PERM_WRITE) != OEMU_OK) {
    return -OEMU_EFAULT;
  }
  if (oemu_memory_write(mem, ts_gva, OEMU_MEM_DWORD, (uint64_t)ts.tv_sec) != OEMU_OK ||
      oemu_memory_write(mem, ts_gva + 8ULL, OEMU_MEM_DWORD, (uint64_t)ts.tv_nsec) != OEMU_OK) {
    return -OEMU_EFAULT;
  }
  return 0;
}

int64_t oemu_sysenv_syscall(oemu_sysenv *env, oemu_memory *mem, uint64_t nr,
                            const uint64_t args[6]) {
  if (env == NULL || args == NULL) {
    return -OEMU_EINVAL;
  }
  switch (nr) {
    case OEMU_SYS_EXIT:
    case OEMU_SYS_EXIT_GROUP:
      env->exit_code = (int)(uint32_t)args[0];
      env->exited = true;
      return 0;
    case OEMU_SYS_WRITE:
      return sys_write(env, mem, args[0], args[1], args[2]);
    case OEMU_SYS_CLOCK_GETTIME:
      return sys_clock_gettime(mem, args[0], args[1]);
    default:
      return -OEMU_ENOSYS;
  }
}
