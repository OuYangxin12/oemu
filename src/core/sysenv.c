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

static int64_t sys_write(oemu_sysenv *env, const oemu_memops *mem, uint64_t fd, uint64_t buf,
                         uint64_t count) {
  if (fd != 1ULL && fd != 2ULL) {
    return -OEMU_EBADF;
  }
  if (count == 0ULL) {
    return 0;
  }
  if (mem == NULL) {
    return -OEMU_EFAULT;
  }
  if (count > (UINT64_MAX - buf)) {
    return -OEMU_EFAULT; /* the range wraps; no region can contain it */
  }
  if (mem->validate(mem->ctx, buf, count, OEMU_PERM_READ) != OEMU_OK) {
    return -OEMU_EFAULT;
  }
  /* Read the bytes out of the guest one at a time into a small bounce buffer:
   * the bus seam has no raw-host-pointer entry point, and a syscall is
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
      if (mem->read(mem->ctx, buf + written + i, OEMU_MEM_BYTE, false, &byte) != OEMU_OK) {
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

static int64_t sys_clock_gettime(const oemu_memops *mem, uint64_t clock_id, uint64_t ts_addr) {
  struct timespec ts;
  if (clock_id != OEMU_CLOCK_REALTIME && clock_id != OEMU_CLOCK_MONOTONIC) {
    return -OEMU_EINVAL;
  }
  if (clock_gettime((clockid_t)clock_id, &ts) != 0) {
    return -OEMU_EFAULT;
  }
  if (mem == NULL) {
    return -OEMU_EFAULT;
  }
  /* struct timespec in the guest is two 64-bit fields, seconds then
   * nanoseconds; both happen to match the host on AArch64 LP64. */
  if (mem->validate(mem->ctx, ts_addr, 8ULL, OEMU_PERM_WRITE) != OEMU_OK ||
      mem->validate(mem->ctx, ts_addr + 8ULL, 8ULL, OEMU_PERM_WRITE) != OEMU_OK) {
    return -OEMU_EFAULT;
  }
  if (mem->write(mem->ctx, ts_addr, OEMU_MEM_DWORD, (uint64_t)ts.tv_sec) != OEMU_OK ||
      mem->write(mem->ctx, ts_addr + 8ULL, OEMU_MEM_DWORD, (uint64_t)ts.tv_nsec) != OEMU_OK) {
    return -OEMU_EFAULT;
  }
  return 0;
}

/* The seam-level core: everything below the public entry point works through
 * oemu_memops, so the same table serves a flat oemu_memory guest and, from
 * M2 on, a full-system machine's physical bus. */
static int64_t sysenv_syscall_ops(oemu_sysenv *env, const oemu_memops *mem, uint64_t nr,
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

int64_t oemu_sysenv_syscall(oemu_sysenv *env, oemu_memory *mem, uint64_t nr,
                            const uint64_t args[6]) {
  /* The view is built even for a NULL `mem`: the adapters pass the NULL
   * context through and every entry point rejects it, which is exactly the
   * -EFAULT the old direct call produced. */
  const oemu_memops bus = oemu_memory_memops(mem);
  return sysenv_syscall_ops(env, &bus, nr, args);
}

/* --- environment view ---------------------------------------------------------- */

static int64_t envops_syscall(void *ctx, const oemu_memops *mem, uint64_t nr,
                              const uint64_t args[6]) {
  return sysenv_syscall_ops((oemu_sysenv *)ctx, mem, nr, args);
}

static bool envops_halted(const void *ctx) {
  return oemu_sysenv_exited((const oemu_sysenv *)ctx);
}

oemu_env_ops oemu_sysenv_envops(oemu_sysenv *env) {
  return (oemu_env_ops){.ctx = env, .syscall = envops_syscall, .halted = envops_halted};
}
