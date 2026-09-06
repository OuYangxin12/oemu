/*
 * The virtual CPU: one core executing at any exception level (M2c).
 *
 * oemu_vcpu composes what the modules of M2 were each built to provide:
 * `oemu_cpu` (the EL0-visible register file plus the exclusive monitor),
 * `oemu_sysregs` (banked state, control registers, PSTATE), the exc module
 * (delivery and ERET), and the exec module's system-mode dispatch. It adds
 * the two pieces of machine-level state those modules must not own:
 * interrupt input pins and the instruction quantum that keeps a cooperative
 * scheduler honest (roadmap D2).
 *
 * Everything is embedded by value and nothing allocates: the struct can live
 * on the stack, and dispose is not needed because the bus view and the
 * optional environment point at caller-owned memory that must outlive it.
 *
 * The interrupt pins are level-triggered inputs (what a GIC drives from M4
 * on): `oemu_vcpu_step` checks them before fetching, so delivery lands
 * between instructions as the architecture requires, and a pinned-high IRQ
 * that the handler does not clear re-fires after the ERET -- exactly the
 * behaviour a real level-interrupt controller produces.
 *
 * Faults are never returned in system mode. An unmapped fetch delivers an
 * Instruction Abort, an unallocated encoding delivers Undefined, an
 * unmapped data access delivers a Data Abort with a FAR, and SVC/BRK/HLT
 * reach the guest's own vector table: the status codes below report
 * scheduler-level events, not guest state.
 */
#ifndef OEMU_VCPU_H
#define OEMU_VCPU_H

#include "oemu/exec.h"
#include "oemu/macros.h"
#include "oemu/memops.h"
#include "oemu/mmu.h"
#include "oemu/status.h"
#include "oemu/sysreg.h"

#include <stdbool.h>
#include <stdint.h>

OEMU_BEGIN_DECLS

typedef struct oemu_vcpu {
  oemu_cpu cpu;            /* register file, exclusive monitor, EL0 thread ID */
  oemu_sysregs sysregs;    /* paired with &cpu.regs by init; do not re-point */
  oemu_memops mem;         /* the physical bus; a copied view, ctx is caller-owned */
  oemu_mmu mmu;            /* the translation layer over `mem`; initialized with
                            * it, and the only bus the step path uses --
                            * SCTLR_EL1.M decides whether it is identity or a
                            * real walk */
  const oemu_env_ops *env; /* nullable; only `halted` is consulted in system
                            * mode -- SVC is an exception here, not a syscall */

  /* Level-triggered interrupt inputs. Devices drive them (the GIC from M4);
   * delivery honours the I and F bits of PSTATE. */
  bool irq_level;
  bool fiq_level;
  /* The event register that SEV sets and WFE consumes. */
  bool event_reg;

  /* Cooperative scheduling (roadmap D2): step refuses to execute once the
   * countdown is spent; oemu_vcpu_rearm reloads it at the quantum boundary. */
  uint64_t quantum;
  uint64_t insns_left;
} oemu_vcpu;

/*
 * Boots one vCPU at `el`: PC and SP take the entry values, the sysreg bank
 * takes its reset table, PSTATE boots with all interrupts masked, and the
 * quantum is armed. EL0 is accepted (its exceptions route to EL1, which is
 * what a lower-EL smoke guest exercises); EL2 aborts inside the sysreg
 * module -- oemu implements no EL2 -- and is refused here as
 * OEMU_ERR_INVALID_ARG, as are NULL arguments, a half-built bus view and a
 * zero quantum.
 */
OEMU_NODISCARD oemu_status oemu_vcpu_init(oemu_vcpu *vcpu, const oemu_memops *mem,
                                          const oemu_env_ops *env, oemu_el el,
                                          uint64_t entry_pc, uint64_t entry_sp,
                                          uint64_t quantum);

/* The current exception level, read from PSTATE. */
oemu_el oemu_vcpu_el(const oemu_vcpu *vcpu);

/* Drive one interrupt pin. Level semantics: the interrupt is pending while
 * the pin is high, and delivery does not clear the pin. */
void oemu_vcpu_set_irq(oemu_vcpu *vcpu, bool level);
void oemu_vcpu_set_fiq(oemu_vcpu *vcpu, bool level);

/*
 * Deliver a pending, unmasked interrupt if one exists (FIQ beats IRQ).
 * Returns true when an exception was taken. Exposed because a scheduler may
 * want to interpose between the check and the step; oemu_vcpu_step calls it
 * itself, so ordinary callers do not need to.
 */
bool oemu_vcpu_take_pending(oemu_vcpu *vcpu);

/*
 * One instruction. Checks the interrupt pins first (precise: the interrupt
 * is taken before the fetched instruction runs, and ELR names that
 * instruction), fetches, decodes, and executes with system-mode semantics.
 * `insn_out` receives the decoded instruction whenever decoding succeeded,
 * including when the step then delivered its exception.
 *
 * OEMU_OK:   progress -- an instruction executed, an exception was delivered
 *            (both count against the quantum).
 * OEMU_ERR_BLOCKED: WFI/WFE with nothing to wake for. Nothing was consumed:
 *            PC, flags and the countdown are untouched; the scheduler should
 *            run another vCPU or deliver an interrupt and step again.
 * OEMU_ERR_TIMEOUT: the quantum is spent.
 * OEMU_ERR_INVALID_ARG: caller bug.
 */
OEMU_NODISCARD oemu_status oemu_vcpu_step(oemu_vcpu *vcpu, oemu_insn *insn_out);

/*
 * Steps until the environment halts (OEMU_OK), the budget or quantum is spent
 * (OEMU_ERR_TIMEOUT, *completed_out counts the steps that made progress), or
 * a WFI/WFE parks the core (OEMU_ERR_BLOCKED, resumable). A guest asking to
 * stop needs a device or PSCI to say so; until M4, an env whose `halted`
 * never returns true simply runs to its budget.
 */
OEMU_NODISCARD oemu_status oemu_vcpu_run(oemu_vcpu *vcpu, uint64_t max_insns,
                                         uint64_t *completed_out);

/* Reload the countdown after a scheduler has given the core a new slice. */
void oemu_vcpu_rearm(oemu_vcpu *vcpu);

OEMU_END_DECLS

#endif /* OEMU_VCPU_H */
