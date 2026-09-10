/*
 * The virtual CPU. See include/oemu/vcpu.h for the contract and
 * src/vcpu/vcpu_internal.h for the pure interrupt decision.
 *
 * The layering rule this file exists to enforce: the exec module knows
 * instructions, the exc module knows exception records, and NEITHER may know
 * about interrupt pins or scheduler quotas. All machine-level policy --
 * when to take an interrupt, when WFI means sleep, when the slice is over --
 * lives here, between them.
 */

#include "oemu/vcpu.h"

#include "oemu/check.h"
#include "oemu/decode.h"
#include "oemu/exc.h"
#include "oemu/regs.h"

#include <stddef.h>

#include "exec/exec_internal.h"
#include "vcpu_internal.h"

/* Bit positions inside DAIF as stored in PSTATE: D9 A8 I7 F6. */
#define VCPU_I_MASK ((uint64_t)1U << 7U)
#define VCPU_F_MASK ((uint64_t)1U << 6U)

int oemu_vcpu_internal_pending_kind(uint64_t pstate, bool irq_level, bool fiq_level) {
  /* FIQ is architecturally higher priority than IRQ. A masked pin is not
   * pending at all: it neither delivers nor wakes. */
  if (fiq_level && ((pstate & VCPU_F_MASK) == 0U)) {
    return OEMU_EXC_KIND_FIQ;
  }
  if (irq_level && ((pstate & VCPU_I_MASK) == 0U)) {
    return OEMU_EXC_KIND_IRQ;
  }
  return -1;
}

oemu_status oemu_vcpu_init(oemu_vcpu *vcpu, const oemu_memops *mem, const oemu_env_ops *env,
                           oemu_el el, uint64_t entry_pc, uint64_t entry_sp, uint64_t quantum) {
  if ((vcpu == NULL) || (mem == NULL) || (el == OEMU_EL2) || (quantum == 0U)) {
    return OEMU_ERR_INVALID_ARG;
  }
  /* The step path calls every callback; a half-built view is a caller bug and
   * is reported here rather than through a NULL function pointer later. */
  if ((mem->fetch32 == NULL) || (mem->read == NULL) || (mem->write == NULL) ||
      (mem->validate == NULL)) {
    return OEMU_ERR_INVALID_ARG;
  }

  *vcpu = (oemu_vcpu){0};
  const oemu_status cpu = oemu_cpu_init(&vcpu->cpu, entry_pc, entry_sp);
  if (cpu != OEMU_OK) {
    return cpu;
  }
  oemu_sysregs_init(&vcpu->sysregs, &vcpu->cpu.regs, el);
  vcpu->mem = *mem;
  oemu_mmu_init(&vcpu->mmu, &vcpu->sysregs, mem);
  vcpu->env = env;
  vcpu->quantum = quantum;
  vcpu->insns_left = quantum;
  return OEMU_OK;
}

oemu_el oemu_vcpu_el(const oemu_vcpu *vcpu) {
  OEMU_REQUIRE(vcpu != NULL, "NULL oemu_vcpu");
  return oemu_pstate_el(vcpu->sysregs.pstate);
}

void oemu_vcpu_set_irq(oemu_vcpu *vcpu, bool level) {
  OEMU_REQUIRE(vcpu != NULL, "NULL oemu_vcpu");
  vcpu->irq_level = level;
}

void oemu_vcpu_set_fiq(oemu_vcpu *vcpu, bool level) {
  OEMU_REQUIRE(vcpu != NULL, "NULL oemu_vcpu");
  vcpu->fiq_level = level;
}

bool oemu_vcpu_take_pending(oemu_vcpu *vcpu) {
  OEMU_REQUIRE(vcpu != NULL, "NULL oemu_vcpu");
  const int kind =
      oemu_vcpu_internal_pending_kind(vcpu->sysregs.pstate, vcpu->irq_level, vcpu->fiq_level);
  if (kind < 0) {
    return false;
  }
  const oemu_el cur = oemu_pstate_el(vcpu->sysregs.pstate);
  /* Interrupts take no syndrome: ESR keeps whatever the last sync exception
   * wrote (the exc module leaves it untouched for the interrupt kinds). */
  oemu_exc_take(&vcpu->cpu.regs, &vcpu->sysregs, (oemu_exc_kind)kind, oemu_exc_route(cur), 0U,
                0U, false);
  return true;
}

/* WFI/WFE (D2). A wake needs no condition oemu can model as a sleep-cause:
 * pending-but-masked is woken anyway (the interrupt cannot be unmasked by a
 * halted core, and the architecture permits spurious wake-ups -- QEMU's WFI
 * helper applies the same mask-free test), and an armed event register wakes
 * WFE only. Otherwise the core parks: PC, flags and the countdown are
 * untouched, and the scheduler owns the wake-up. */
static oemu_status vcpu_wait(oemu_vcpu *vcpu, const oemu_insn *in) {
  const bool wake =
      vcpu->irq_level || vcpu->fiq_level || ((in->op == OEMU_OP_WFE) && vcpu->event_reg);
  if (!wake) {
    return OEMU_ERR_BLOCKED;
  }
  if (in->op == OEMU_OP_WFE) {
    vcpu->event_reg = false; /* the one event is spent */
  }
  /* Wake: the wait executes as a no-op -- which is also exactly what the
   * shared switch does, so the caller lets it fall through. */
  return OEMU_OK;
}

/* Whether the environment says the guest has stopped. A missing environment
 * or callback reads as "still running", the seam's standing convention. */
static bool vcpu_halted(const oemu_vcpu *vcpu) {
  return (vcpu->env != NULL) && (vcpu->env->halted != NULL) &&
         vcpu->env->halted(vcpu->env->ctx);
}

oemu_status oemu_vcpu_step(oemu_vcpu *vcpu, oemu_insn *insn_out) {
  if (vcpu == NULL) {
    return OEMU_ERR_INVALID_ARG;
  }
  if (vcpu->insns_left == 0U) {
    return OEMU_ERR_TIMEOUT; /* the scheduler owns the re-arm */
  }

  /* The generic timer's counter advances with guest progress, one count per
   * retired instruction: the modelled core runs at exactly the rate the guest
   * is told (OEMU_CNTFRQ_EL0_DEFAULT, 62.5 MHz, which is what CNTFRQ_EL0
   * reports and what arch_timer prints), so the counter is not merely monotonic
   * but *interpretable* -- a count delta means the same span of guest time to
   * the guest that it means to us.
   *
   * The old step was 1000000 counts per instruction, chosen so that a busy-wait
   * would retire in a handful of steps. That made the counter 16 million times
   * faster than the frequency the guest was told, and the guest is entitled to
   * use that number: at 62.5 MHz the kernel's one-jiffy comparator delta is
   * 625000 counts, which the old step satisfied with a single instruction, so
   * every tick fired the instant it was armed (measured: ~5600 IRQ deliveries
   * per 150M instructions), calibrate_delay() calibrated the loops against a
   * clock that moved that way, and the guest's whole sense of time -- RCU
   * grace periods, watchdows, mdelay(), the timer wheel -- ran on it. A
   * delay loop is then simply an honest O(n) busy wait, which is what it is on
   * real hardware too. */
  vcpu->sysregs.cntvct += OEMU_TIMER_COUNTS_PER_INSN;

  /* Interrupts are taken before the fetch, so ELR names the instruction that
   * was about to run (precise, and the instruction re-executes after the
   * ERET). Delivery is machine progress even though no instruction retired,
   * and charging it to the quantum is what stops a pin the handler does not
   * clear from spinning the core forever. */
  if (oemu_vcpu_take_pending(vcpu)) {
    vcpu->insns_left--;
    return OEMU_OK;
  }

  /* The step path always runs through the translation layer: with the MMU
   * off or at EL3 it is exactly the bus beneath it, and it is the only place
   * that knows whether a failed access is worth a syndrome record. */
  const oemu_memops view = oemu_mmu_memops(&vcpu->mmu);

  uint32_t word = 0U;
  const uint64_t pc = oemu_regs_pc(&vcpu->cpu.regs);
  if (view.fetch32(view.ctx, pc, &word) != OEMU_OK) {
    /* Instruction Abort, FAR = the PC the core was trying to run. The mmu
     * record (when present) carries the walk's verdict: the real class and
     * level, not the flat-bus approximation. */
    oemu_mmu_fault fault;
    if (oemu_mmu_take_fault(&vcpu->mmu, &fault)) {
      oemu_exc_take(&vcpu->cpu.regs, &vcpu->sysregs, OEMU_EXC_KIND_SYNC,
                    oemu_exc_route(oemu_vcpu_el(vcpu)), fault.esr, fault.far, true);
    } else {
      oemu_exc_instruction_abort(&vcpu->cpu.regs, &vcpu->sysregs, pc);
    }
    vcpu->insns_left--;
    return OEMU_OK;
  }

  oemu_insn insn;
  const oemu_status dec = oemu_decode(word, pc, &insn);
  if (insn_out != NULL) {
    *insn_out = insn; /* zeroed by the decoder on failure: safe to expose */
  }
  if (dec != OEMU_OK) {
    /* Unallocated (DECODE) or deliberately-absent (UNSUPPORTED: SIMD,
     * atomics -- the ID registers advertise them as not implemented): the
     * guest gets an Undefined carrying the full encoding. */
    oemu_exc_undefined(&vcpu->cpu.regs, &vcpu->sysregs, word);
    vcpu->insns_left--;
    return OEMU_OK;
  }

  /* SEV/SEVL (hint #4/#5) arm the event register the next WFE consumes;
   * both still execute and advance as the hints they are. */
  if ((insn.op == OEMU_OP_HINT) && ((insn.uimm == 4U) || (insn.uimm == 5U))) {
    vcpu->event_reg = true;
  }

  /* Only the vCPU knows the pins and the event register, so the wait-or-park
   * decision is taken here, not inside the shared switch. */
  if ((insn.op == OEMU_OP_WFI) || (insn.op == OEMU_OP_WFE)) {
    const oemu_status wait = vcpu_wait(vcpu, &insn);
    if (wait != OEMU_OK) {
      return wait;
    }
  }

  const oemu_status st = oemu_exec_internal_dispatch_system(&vcpu->cpu, &vcpu->sysregs, &view,
                                                            &insn, word, &vcpu->mmu, vcpu->env);
  if (st != OEMU_OK) {
    return st; /* BLOCKED (backstop: unreachable from here) or INVALID_ARG */
  }

  vcpu->insns_left--;
  return OEMU_OK;
}

oemu_status oemu_vcpu_run(oemu_vcpu *vcpu, uint64_t max_insns, uint64_t *completed_out) {
  uint64_t done = 0U;
  while (done < max_insns) {
    if (vcpu_halted(vcpu)) {
      break;
    }
    const oemu_status st = oemu_vcpu_step(vcpu, NULL);
    if (st != OEMU_OK) {
      if (completed_out != NULL) {
        *completed_out = done;
      }
      return st; /* TIMEOUT (quantum or budget) and BLOCKED both propagate */
    }
    done++;
  }
  if (completed_out != NULL) {
    *completed_out = done;
  }
  return vcpu_halted(vcpu) ? OEMU_OK : OEMU_ERR_TIMEOUT;
}

void oemu_vcpu_rearm(oemu_vcpu *vcpu) {
  OEMU_REQUIRE(vcpu != NULL, "NULL oemu_vcpu");
  vcpu->insns_left = vcpu->quantum;
}
