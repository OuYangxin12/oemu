/*
 * The instruction executor: fetch, decode, dispatch, repeat.
 *
 * One oemu_cpu is one single-threaded AArch64 core running at EL0: the
 * architectural register set plus the two pieces of state that instructions
 * can observe but the regs module deliberately does not model -- the
 * exclusive-access monitor and TPIDRUR_EL0. The struct is not opaque, so it can
 * live on the stack, and no operation on it ever allocates.
 *
 * Fault behaviour is the part to read carefully. oemu_exec_step is precise by
 * contract: when it returns OEMU_ERR_FAULT (bad address, bad permission,
 * misaligned fetch, BRK/HLT) or OEMU_ERR_UNSUPPORTED, the PC still points at
 * the faulting instruction and no architectural register has moved -- even for
 * instructions that touch several locations (STP writeback, LDPSW), which is
 * why every memory reference is validated before the first commit and
 * multi-transfer instructions stage through locals.
 *
 * The loop has no thread and no OS: the guest environment is whatever the
 * oemu_env_ops it is handed implements, which today is oemu_sysenv's
 * four-syscall benchmark ABI.
 */
#ifndef OEMU_EXEC_H
#define OEMU_EXEC_H

#include "oemu/decode.h"
#include "oemu/macros.h"
#include "oemu/memops.h"
#include "oemu/memory.h"
#include "oemu/regs.h"
#include "oemu/status.h"
#include "oemu/sysenv.h"

#include <stdbool.h>
#include <stdint.h>

OEMU_BEGIN_DECLS

typedef struct oemu_cpu {
  oemu_regs regs;
  /*
   * The exclusive monitor, one reservation per core. Any store overlapping
   * the reserved range clears it -- the strict, single-core-exact choice,
   * cheaper to reason about than the architectural "implies unpredictable".
   */
  uint64_t monitor_addr;
  uint64_t monitor_size;
  bool monitor_valid;
  /* Thread ID register: writable, read via MRS, meaningful to the guest. */
  uint64_t tpidrur_el0;
  /*
   * The 128-bit vector register file, V0-V31, as two 64-bit halves:
   * v[n][0] is Vn.D[0] (bits 63:0), v[n][1] is Vn.D[1] (bits 127:64).
   *
   * This exists for one reason: Linux 6.6 calls fpsimd_load_state/fpsimd_save_state,
   * which are LDP/STP of Q0-Q31, on every return to and switch out of user
   * mode, and there is no way to tell it not to -- the guard behind
   * system_supports_fpsimd() is ARM64_HAS_NO_FPSIMD, a capability no CPU
   * feature ever sets in this tree. So the register file has to exist before
   * an /init can run, whatever the ID registers claim.
   *
   * No FP arithmetic is modelled, and none is needed to boot the guest we
   * boot: the kernel touches the state only as opaque 512 bytes, and our
   * /init is compiled without SIMD. Feature registers still say no SIMD;
   * running a NEON-using binary remains the M6 horizon item.
   */
  uint64_t v[32][2];
} oemu_cpu;

/* Zeroes everything except PC and SP, which take the entry values.
 * OEMU_ERR_INVALID_ARG on NULL. */
OEMU_NODISCARD oemu_status oemu_cpu_init(oemu_cpu *cpu, uint64_t entry_pc, uint64_t initial_sp);

/*
 * Read or write one 128-bit vector register. OEMU_ERR_INVALID_ARG on a NULL
 * argument or a register outside V0-V31. The user-mode facade never reaches
 * these: vector accesses are refused there, exactly like system instructions.
 */
#define OEMU_VEC_REGS 32U /* the vector register file: q0..q31 */

OEMU_NODISCARD oemu_status oemu_vec_read(const oemu_cpu *cpu, unsigned n, uint64_t *lo,
                                         uint64_t *hi);
oemu_status oemu_vec_write(oemu_cpu *cpu, unsigned n, uint64_t lo, uint64_t hi);

/*
 * Executes exactly one instruction.
 *
 * When `insn_out` is non-NULL it receives the decoded instruction, filled
 * whenever decoding succeeded -- including when execution then reported
 * OEMU_ERR_FAULT or OEMU_ERR_UNSUPPORTED -- so a caller can show the offending
 * instruction. Returns OEMU_OK when the instruction completed (for SVC, that
 * includes the syscall having been dispatched; check
 * oemu_sysenv_exited to learn whether the guest asked to stop), the decode
 * module's status on a bad encoding, or OEMU_ERR_FAULT / OEMU_ERR_UNSUPPORTED
 * as described above. This function performs no allocation.
 */
OEMU_NODISCARD oemu_status oemu_exec_step(oemu_cpu *cpu, oemu_memory *mem, oemu_sysenv *env,
                                          oemu_insn *insn_out);

/*
 * Runs until the guest exits (OEMU_OK -- query oemu_sysenv_exited), the budget
 * is spent (OEMU_ERR_TIMEOUT; *completed_out counts the instructions that did
 * run, and the machine remains exactly at the next unexecuted instruction),
 * or a fault/unsupported/decode error surfaces (that status; state precise).
 * `completed_out` may be NULL.
 */
OEMU_NODISCARD oemu_status oemu_exec_run(oemu_cpu *cpu, oemu_memory *mem, oemu_sysenv *env,
                                         uint64_t max_insns, uint64_t *completed_out);

/*
 * The bus-seam entries: the same two contracts, over any oemu_memops
 * (oemu_memory_memops, oemu_aspace_memops, or a machine's own bus once M2
 * lands) and any oemu_env_ops (oemu_sysenv_envops today, a PSCI-driven one
 * later). `mem` must be non-NULL with all four callbacks set -- a view
 * missing one is a caller bug, reported as OEMU_ERR_INVALID_ARG rather than
 * called through. `env` may be NULL, in which case an SVC reports
 * OEMU_ERR_UNSUPPORTED exactly as before and the run loop's halted check
 * reads as false. The two entry points above are thin wrappers over these.
 */
OEMU_NODISCARD oemu_status oemu_exec_step_bus(oemu_cpu *cpu, const oemu_memops *mem,
                                              const oemu_env_ops *env, oemu_insn *insn_out);

OEMU_NODISCARD oemu_status oemu_exec_run_bus(oemu_cpu *cpu, const oemu_memops *mem,
                                             const oemu_env_ops *env, uint64_t max_insns,
                                             uint64_t *completed_out);

/* Print the tracer's PC histogram -- where the guest was spending its
 * instructions -- to `out`, reporting at most `top` entries, and return how many
 * were printed. A no-op returning 0 unless OEMU_TRACE_HIST armed the sampler, so
 * a run loop can call it unconditionally when it gives up on a boot. */
unsigned oemu_exec_trace_report(FILE *out, unsigned top);

OEMU_END_DECLS

#endif /* OEMU_EXEC_H */
