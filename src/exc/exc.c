/*
 * Exception entry and return. See oemu/exc.h for the contract; every bit
 * layout comment here cites QEMU's syndrome.h/arm_cpu_do_interrupt_aarch64
 * or Linux's esr.h, which encode the same ARM ARM rules.
 */
#include "oemu/exc.h"

#include "oemu/check.h"
#include "oemu/regs.h"
#include "oemu/sysreg.h"

#include <stddef.h>

#include "exc_internal.h"

/* --- ESR builders ------------------------------------------------------------ */

uint32_t oemu_exc_internal_esr(oemu_exc_ec ec, uint32_t iss) {
  /* ESR[31:26] = EC, [25] = IL (1: raised by a 32-bit instruction), [24:0] =
   * ISS. QEMU sets IL the same way for every AArch64 source. */
  return (((uint32_t)ec) << 26) | (1U << 25) | (iss & 0x01FFFFFFU);
}

uint32_t oemu_exc_internal_esr_undefined(uint32_t insn) {
  return oemu_exc_internal_esr(OEMU_EXC_EC_UNKNOWN, insn & 0x01FFFFFFU);
}

uint32_t oemu_exc_internal_esr_imm16(oemu_exc_ec ec, uint32_t imm16) {
  return oemu_exc_internal_esr(ec, imm16 & 0xFFFFU);
}

uint32_t oemu_exc_internal_esr_data_abort(oemu_exc_ec ec, unsigned log2_size, bool is_write,
                                          bool isv, uint8_t dfsc) {
  /* DABORT_ISS layout per QEMU syndrome.h FIELD()s: DFSC[5:0], WnR[6],
   * SET[12:11], SAS[23:21], ISV[24]. */
  uint32_t iss = dfsc & 0x3FU;
  if (isv) {
    iss |= 1U << 24;                          /* ISV: transfer details present */
    iss |= (uint32_t)(log2_size & 3U) << 21U; /* SAS */
    iss |= 1U << 11;                          /* SET = 0b01: whole access */
  }
  if (is_write) {
    iss |= 1U << 6; /* WnR */
  }
  return oemu_exc_internal_esr(ec, iss);
}

uint32_t oemu_exc_internal_esr_instruction_abort(oemu_exc_ec ec, uint8_t dfsc) {
  return oemu_exc_internal_esr(ec, (uint32_t)dfsc & 0x3FU);
}

uint32_t oemu_exc_internal_esr_illegal_eret(void) {
  return oemu_exc_internal_esr(OEMU_EXC_EC_ILLEGAL_ERET, 0);
}

/* --- mode and routing -------------------------------------------------------- */

bool oemu_exc_internal_mode_valid(uint64_t mode) {
  /* Implemented AArch64 states only: bit 4 is the AArch32 flag, bit 1 is
   * RES0, and EL0 exists only as EL0t (mode 0). */
  const uint64_t el = (mode >> 2) & 3U;
  return (mode & 0x12U) == 0 && !(el == 0 && (mode & 1U) != 0);
}

oemu_el oemu_exc_route(oemu_el from) {
  /* An EL3 guest is the top of this model, so its exceptions stay there;
   * everything else is delivered to the kernel's EL1 (roadmap D3). */
  return (from == OEMU_EL3) ? OEMU_EL3 : OEMU_EL1;
}

uint64_t oemu_exc_vector_offset(bool same_el, bool sp_sel, oemu_exc_kind kind) {
  /* Base group, then the 128-byte-per-kind stride. The AArch32 lower-EL
   * group (0x600) never applies: oemu guests are AArch64. */
  uint64_t group = 0x400U;
  if (same_el) {
    group = sp_sel ? 0x200U : 0x000U;
  }
  return group + ((uint64_t)kind << 7);
}

/* --- entry and return ---------------------------------------------------------- */

void oemu_exc_take(oemu_regs *regs, oemu_sysregs *sysregs, oemu_exc_kind kind, oemu_el target,
                   uint32_t esr, uint64_t far, bool far_valid) {
  OEMU_REQUIRE(regs != NULL, "NULL oemu_regs");
  OEMU_REQUIRE(sysregs != NULL, "NULL oemu_sysregs");
  /* Delivery to EL0 is not a thing in the AArch64 model, and EL2 does not
   * exist to deliver to. */
  OEMU_REQUIRE(target != OEMU_EL0 && target != OEMU_EL2, "exception target must be EL1 or EL3");

  const uint64_t old_pstate = sysregs->pstate;
  const oemu_el from = oemu_pstate_el(old_pstate);
  /* Compute the vector before clobbering anything: it depends only on state
   * the interrupted world still holds. */
  const uint64_t vector =
      sysregs->vbar_el[target] +
      oemu_exc_vector_offset(from == target, oemu_pstate_sp_sel(old_pstate) != 0U, kind);

  /* Entry PSTATE: h-mode of the target (entry always selects SP_ELx), all
   * four interrupt masks set, IL=1. DAIF is taken from QEMU's entry path --
   * it writes DAIF=0b1111 on every exception type; the ARM ARM's sync
   * variant preserves I/F, a distinction only a guest that relies on
   * in-handler unmasking would notice, and the kernel entry code masks
   * first thing either way. */
  const uint64_t entry_pstate = oemu_pstate_mode(target) |
                                (OEMU_PSTATE_DAIF_MASK << OEMU_PSTATE_DAIF_SHIFT) |
                                OEMU_PSTATE_IL;
  /* Bank switch first (it still sees the interrupted PSTATE as "from"), then
   * record the interrupted world in the target's banks. */
  oemu_sysregs_switch_sp(sysregs, entry_pstate);

  sysregs->spsr_el[target] = old_pstate;
  /* ELR = the faulting instruction's address: callers invoke this before the
   * PC advances, so regs->pc still names it (precise-exception contract). */
  sysregs->elr_el[target] = regs->pc;
  if (kind == OEMU_EXC_KIND_SYNC || kind == OEMU_EXC_KIND_SERROR) {
    sysregs->esr_el[target] = esr;
  }
  if (far_valid) {
    sysregs->far_el[target] = far;
  }

  regs->pc = vector;
}

void oemu_exc_eret(oemu_regs *regs, oemu_sysregs *sysregs) {
  OEMU_REQUIRE(regs != NULL, "NULL oemu_regs");
  OEMU_REQUIRE(sysregs != NULL, "NULL oemu_sysregs");

  const oemu_el cur = oemu_pstate_el(sysregs->pstate);
  /* ERET is an EL1+ instruction; the executor is supposed to reject it at
   * EL0, but delivering Undefined here keeps the module honest in isolation. */
  if (cur == OEMU_EL0) {
    oemu_exc_undefined(regs, sysregs, 0xD69F03E0U);
    return;
  }
  const uint64_t saved = sysregs->spsr_el[cur];

  /* An SPSR that would return into an illegal state is itself the exception
   * (EC 0b011010): the classic stack-smash tripwire for kernels. */
  if ((saved & OEMU_PSTATE_IL) != 0) {
    oemu_exc_take(regs, sysregs, OEMU_EXC_KIND_SYNC, cur, oemu_exc_internal_esr_illegal_eret(),
                  0, false);
    return;
  }

  const uint64_t mode = saved & OEMU_PSTATE_M_MASK;
  const oemu_el target = oemu_pstate_el(mode);
  if (!oemu_exc_internal_mode_valid(mode) || target > cur) {
    /* Returning upward, to AArch32, or to a reserved mode: undefined
     * instruction, to the level that tried it. ISS = the ERET encoding
     * (constant for the plain form QEMU and clang both use). */
    oemu_exc_take(regs, sysregs, OEMU_EXC_KIND_SYNC, cur,
                  oemu_exc_internal_esr_undefined(0xD69F03E0U), 0, false);
    return;
  }

  /* Restore: the shared switch saves the pre-ERET SP into its bank, loads
   * the returned-to bank, and adopts the restored PSTATE in one step. */
  oemu_sysregs_switch_sp(sysregs, saved);
  regs->pc = sysregs->elr_el[cur];
}

/* --- fault sources ------------------------------------------------------------- */

void oemu_exc_undefined(oemu_regs *regs, oemu_sysregs *sysregs, uint32_t insn) {
  oemu_exc_take(regs, sysregs, OEMU_EXC_KIND_SYNC,
                oemu_exc_route(oemu_pstate_el(sysregs->pstate)),
                oemu_exc_internal_esr_undefined(insn), 0, false);
}

void oemu_exc_svc(oemu_regs *regs, oemu_sysregs *sysregs, uint16_t imm16) {
  oemu_exc_take(regs, sysregs, OEMU_EXC_KIND_SYNC,
                oemu_exc_route(oemu_pstate_el(sysregs->pstate)),
                oemu_exc_internal_esr_imm16(OEMU_EXC_EC_SVC64, imm16), 0, false);
}

void oemu_exc_brk(oemu_regs *regs, oemu_sysregs *sysregs, uint16_t imm16) {
  oemu_exc_take(regs, sysregs, OEMU_EXC_KIND_SYNC,
                oemu_exc_route(oemu_pstate_el(sysregs->pstate)),
                oemu_exc_internal_esr_imm16(OEMU_EXC_EC_BRK64, imm16), 0, false);
}

void oemu_exc_smc(oemu_regs *regs, oemu_sysregs *sysregs, uint16_t imm16) {
  /* Until M4 installs PSCI the monitor cannot service calls, and the
   * architectural response to an unsupported monitor call is Undefined.
   * The exception word is reconstructed from the immediate so the ISS points
   * at the instruction that faulted (SVC/HVC/SMC share the layout, only the
   * low two bits differ -- QEMU's a64.decode agrees). */
  oemu_exc_undefined(regs, sysregs, 0xD4000003U | ((uint32_t)imm16 << 5));
}

void oemu_exc_hvc(oemu_regs *regs, oemu_sysregs *sysregs, uint16_t imm16) {
  oemu_exc_undefined(regs, sysregs, 0xD4000002U | ((uint32_t)imm16 << 5));
}

void oemu_exc_breakpoint(oemu_regs *regs, oemu_sysregs *sysregs) {
  /* Debug-exception ISS (QEMU's arm_debug_exception agrees): ISV=1, IDS=0,
   * DFSC=0b000100 software trigger. */
  oemu_exc_take(regs, sysregs, OEMU_EXC_KIND_SYNC,
                oemu_exc_route(oemu_pstate_el(sysregs->pstate)),
                oemu_exc_internal_esr(OEMU_EXC_EC_BREAKPOINT, (1U << 24) | 0x04U), 0, false);
}

void oemu_exc_instruction_abort(oemu_regs *regs, oemu_sysregs *sysregs, uint64_t far) {
  const oemu_el from = oemu_pstate_el(sysregs->pstate);
  const oemu_el target = oemu_exc_route(from);
  const oemu_exc_ec ec = (from < target) ? OEMU_EXC_EC_IABORT_LOWER : OEMU_EXC_EC_IABORT_SAME;
  /* DFSC 0b101100: translation fault, level -1 (QEMU uses it for
   * "address not covered by any mapping", which is what an unmapped aspace
   * range is). */
  oemu_exc_take(regs, sysregs, OEMU_EXC_KIND_SYNC, target,
                oemu_exc_internal_esr_instruction_abort(ec, 0x2CU), far, true);
}

void oemu_exc_data_abort(oemu_regs *regs, oemu_sysregs *sysregs, uint64_t far,
                         unsigned log2_size, bool is_write, bool isv, uint8_t dfsc) {
  const oemu_el from = oemu_pstate_el(sysregs->pstate);
  const oemu_el target = oemu_exc_route(from);
  const oemu_exc_ec ec = (from < target) ? OEMU_EXC_EC_DABORT_LOWER : OEMU_EXC_EC_DABORT_SAME;
  oemu_exc_take(regs, sysregs, OEMU_EXC_KIND_SYNC, target,
                oemu_exc_internal_esr_data_abort(ec, log2_size, is_write, isv, dfsc), far,
                true);
}

const char *oemu_exc_ec_name(oemu_exc_ec ec) {
  switch (ec) {
    case OEMU_EXC_EC_UNKNOWN:
      return "Unknown";
    case OEMU_EXC_EC_WFX:
      return "WFX";
    case OEMU_EXC_EC_FP_ASIMD:
      return "FP_ASIMD";
    case OEMU_EXC_EC_SVC64:
      return "SVC64";
    case OEMU_EXC_EC_HVC64:
      return "HVC64";
    case OEMU_EXC_EC_SMC64:
      return "SMC64";
    case OEMU_EXC_EC_SYSREG:
      return "SYSREG";
    case OEMU_EXC_EC_ILLEGAL_ERET:
      return "IllegalERET";
    case OEMU_EXC_EC_IABORT_LOWER:
      return "IAbortLower";
    case OEMU_EXC_EC_IABORT_SAME:
      return "IAbortSame";
    case OEMU_EXC_EC_DABORT_LOWER:
      return "DAbortLower";
    case OEMU_EXC_EC_DABORT_SAME:
      return "DAbortSame";
    case OEMU_EXC_EC_SERROR:
      return "SError";
    case OEMU_EXC_EC_BREAKPOINT:
      return "Breakpoint";
    case OEMU_EXC_EC_BRK64:
      return "BRK64";
    default:
      return "unknown";
  }
}
