/*
 * System-environment (SVC) surface for the executor.
 *
 * The emulated guest is a freestanding benchmark binary, so the syscall ABI is
 * the small Linux AArch64 set such a binary actually needs -- nothing more.
 * Keeping it tiny is a feature: every number in the table below is the honest
 * Linux value, so a guest built against these numbers is also a guest that
 * runs natively on real hardware, which is how the executor itself gets
 * validated.
 *
 *   number  name            behaviour
 *   ------  ----            ---------
 *   93      exit            terminates, recording the code
 *   94      exit_group      same (a single-threaded model has nothing else to
 *                           retire)
 *   64      write           fd 1/2 go to the host FILE supplied at init
 *   113     clock_gettime   CLOCK_REALTIME / CLOCK_MONOTONIC from the host
 *
 * Failures are returned as negative Linux errnos in x0, the way Linux does:
 * the guest's own error handling then works without learning oemu's codes.
 *
 * The struct is not opaque and the module performs no allocation at all.
 */
#ifndef OEMU_SYSENV_H
#define OEMU_SYSENV_H

#include "oemu/macros.h"
#include "oemu/memory.h"
#include "oemu/status.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

OEMU_BEGIN_DECLS

/* Linux AArch64 syscall numbers used by the guest ABI. */
#define OEMU_SYS_WRITE         ((uint64_t)64U)
#define OEMU_SYS_EXIT          ((uint64_t)93U)
#define OEMU_SYS_EXIT_GROUP    ((uint64_t)94U)
#define OEMU_SYS_CLOCK_GETTIME ((uint64_t)113U)

/* Linux clock ids accepted by clock_gettime. */
#define OEMU_CLOCK_REALTIME  ((uint64_t)0U)
#define OEMU_CLOCK_MONOTONIC ((uint64_t)1U)

/* The subset of Linux errnos this surface can produce, as positive constants;
 * syscall returns negate them. */
#define OEMU_EBADF  9 /* write to a non-standard fd */
#define OEMU_EINVAL 22
#define OEMU_EFAULT 14
#define OEMU_ENOSYS 38

typedef struct oemu_sysenv {
  FILE *out;     /* guest fd 1 and 2 land here; NULL means the host stdout */
  int exit_code; /* valid when exited */
  bool exited;   /* set by exit/exit_group; the run loop checks it */
} oemu_sysenv;

/* Arms the environment as freshly booted: not exited, output to `out`
 * (NULL selects the host stdout). */
void oemu_sysenv_init(oemu_sysenv *env, FILE *out);

/* True once the guest has called exit/exit_group. */
bool oemu_sysenv_exited(const oemu_sysenv *env);

/* The code the guest exited with; 0 until it has exited. */
int oemu_sysenv_exit_code(const oemu_sysenv *env);

/*
 * Executes one syscall: `nr` from x8, arguments x0..x5 in `args`. Returns the
 * value the guest should receive in x0 (a negative errno on failure). Memory
 * operands are validated through `mem`, so a bad guest pointer comes back as
 * -OEMU_EFAULT rather than corrupting the host. Unknown numbers come back as
 * -OEMU_ENOSYS, which likewise leaves guest state untouched apart from x0.
 */
int64_t oemu_sysenv_syscall(oemu_sysenv *env, oemu_memory *mem, uint64_t nr,
                            const uint64_t args[6]);

OEMU_END_DECLS

#endif /* OEMU_SYSENV_H */
