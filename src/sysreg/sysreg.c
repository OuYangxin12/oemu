/*
 * System-register table and the MRS/MSR accessors built on it. See
 * oemu/sysreg.h for the state layout and the trap-vs-status contract.
 */
#include "oemu/sysreg.h"

#include "oemu/check.h"
#include "oemu/gtimer.h"
#include "oemu/macros.h"
#include "oemu/regs.h"
#include "oemu/status.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sysreg_internal.h"

/* --- callbacks for registers whose backing lives outside this struct --------- */

/* CurrentEL reads the level shifted into its field position, bits [3:2]. */
static uint64_t get_current_el(const oemu_sysregs *sr) {
  const oemu_el el = oemu_pstate_el(sr->pstate);
  return (uint64_t)el << 2;
}

static uint64_t get_sp_sel(const oemu_sysregs *sr) {
  return sr->pstate & OEMU_PSTATE_SPSEL;
}

static void set_sp_sel(oemu_sysregs *sr, uint64_t value) {
  /* MSR SPSel really switches stacks: the newly selected bank becomes the
   * interpreter's SP. Delegating to the shared switch keeps this identical
   * to what exception entry/return do. */
  oemu_sysregs_switch_sp(sr, (sr->pstate & ~OEMU_PSTATE_SPSEL) | (value & OEMU_PSTATE_SPSEL));
}

/* MRS Xt, DAIF reports the field where it sits in PSTATE (bits [9:6]) and MSR
 * DAIF, Xt takes the mask at those same positions, exactly as MRS Xt, NZCV uses
 * bits [31:28]. Linux's IRQ entry depends on that: `mov x2, #0xc0; msr daif, x2`
 * must leave I and F masked for the handler to run. Reading the written value as
 * a compact 4-bit field turns 0xc0 into 0 -- unmasking the very interrupt the
 * guest just masked, which re-enters the handler forever. */
static uint64_t get_daif(const oemu_sysregs *sr) {
  return sr->pstate & (OEMU_PSTATE_DAIF_MASK << OEMU_PSTATE_DAIF_SHIFT);
}

static void set_daif(oemu_sysregs *sr, uint64_t value) {
  const uint64_t field = OEMU_PSTATE_DAIF_MASK << OEMU_PSTATE_DAIF_SHIFT;
  sr->pstate = (sr->pstate & ~field) | (value & field);
}

void oemu_sysregs_switch_sp(oemu_sysregs *sr, uint64_t new_pstate) {
  OEMU_REQUIRE(sr != NULL, "NULL oemu_sysregs");
  OEMU_REQUIRE(sr->regs != NULL, "NULL oemu_regs in oemu_sysregs_switch_sp");

  const oemu_el old_el = oemu_pstate_el(sr->pstate);
  const oemu_el new_el = oemu_pstate_el(new_pstate);
  /* Which bank `regs->sp` holds under the current PSTATE, and which it must
   * hold under the new one: SPSel=0 or EL0 both mean SP_EL0. */
  const bool old_user = oemu_pstate_sp_sel(sr->pstate) == 0U || old_el == OEMU_EL0;
  const bool new_user = oemu_pstate_sp_sel(new_pstate) == 0U || new_el == OEMU_EL0;

  if (old_user != new_user || (!old_user && old_el != new_el)) {
    /* Crossing banks: deposit the live value into the bank it came from,
     * withdraw the new one. Staying in one bank (e.g. ERET within EL1h) is
     * a no-op for the SP itself. */
    sr->sp_el[old_user ? OEMU_EL0 : old_el] = sr->regs->sp;
    sr->regs->sp = sr->sp_el[new_user ? OEMU_EL0 : new_el];
  }
  sr->pstate = new_pstate;
}

/* NZCV and SP_EL0 live in the paired oemu_regs; see oemu/sysreg.h. NZCV keeps
 * only the four flag bits on write (the architectural RES0 upper half of the
 * register and the EL0-reserved low bits are dropped, matching oemu_regs). */
static uint64_t get_nzcv(const oemu_sysregs *sr) {
  return (uint64_t)oemu_regs_nzcv(sr->regs);
}

static void set_nzcv(oemu_sysregs *sr, uint64_t value) {
  oemu_regs_set_nzcv(sr->regs, (uint32_t)value);
}

/*
 * Banked stack pointers (SP_EL0, SP_EL1).
 *
 * The active bank's live value is `regs->sp` -- the interpreter's SP -- and
 * inactive banks live in sp_el[]. A guest MRS/MSR must observe the
 * single-bank model the hardware presents: reads answer with the bank the
 * current PSTATE sees, and a write to the active bank moves the
 * interpreter's SP with it (Linux's head.S seeds SP_EL1 before selecting it;
 * KVM restores a vCPU's SP_EL1 through this table while SP_EL1 is that
 * vCPU's active stack).
 */

/* SP_EL0 is active at EL0 and above EL0 whenever SPSel is 0. */
static bool sp_el0_active(const oemu_sysregs *sr) {
  return oemu_pstate_el(sr->pstate) == OEMU_EL0 || oemu_pstate_sp_sel(sr->pstate) == 0U;
}

static uint64_t get_sp_el0(const oemu_sysregs *sr) {
  return sp_el0_active(sr) ? sr->regs->sp : sr->sp_el[OEMU_EL0];
}

static void set_sp_el0(oemu_sysregs *sr, uint64_t value) {
  sr->sp_el[OEMU_EL0] = value;
  if (sp_el0_active(sr)) {
    sr->regs->sp = value;
  }
}

static uint64_t get_sp_el1(const oemu_sysregs *sr) {
  const bool active =
      oemu_pstate_el(sr->pstate) == OEMU_EL1 && oemu_pstate_sp_sel(sr->pstate) != 0U;
  return active ? sr->regs->sp : sr->sp_el[OEMU_EL1];
}

static void set_sp_el1(oemu_sysregs *sr, uint64_t value) {
  sr->sp_el[OEMU_EL1] = value;
  if (oemu_pstate_el(sr->pstate) == OEMU_EL1 && oemu_pstate_sp_sel(sr->pstate) != 0U) {
    sr->regs->sp = value;
  }
}

/* --- the table ----------------------------------------------------------------- */
/*
 * Ascending by sel (the white-box test enforces it). min_el encodes the
 * architecture's accessibility, which is per-register rather than derivable
 * from the encoding (the ID space is op1=0 yet EL0-readable for read, while
 * SPSel at the same op1 is EL1-only), so it is curated per row. Full-width
 * rows carry a ~0 write mask; registers with reserved bits get tighter masks
 * as oemu starts enforcing them (M3).
 */
/* Generic-timer virtual/physical counters: a monotonic count (advanced by the
 * step loop, so a busy delay loop terminates) minus the EL1 virtual offset. The
 * value is computed rather than stored behind an offset because CNTVCT and the
 * timer's TVAL both derive from the live counter and the offset. */
static uint64_t get_cntvct(const oemu_sysregs *sr) {
  return sr->cntvct - sr->cntvoff_el1;
}
static uint64_t get_cntpct(const oemu_sysregs *sr) {
  return sr->cntvct;
}
static uint64_t get_cntp_tval(const oemu_sysregs *sr) {
  return sr->cntp_cval_el1 - sr->cntvct;
}
/* CNTV_CTL/CNTP_CTL bit 2 (ISTAT) is read-only and reflects the live timer
 * condition, exactly the predicate the PPI refresh uses. Linux's arch timer
 * handler reads it to decide whether the interrupt is its own and only then
 * reprograms the comparator; with ISTAT stuck at zero the handler answered
 * IRQ_NONE, nothing re-armed, and the PPI stayed pending for the rest of the
 * boot -- the guest spent every instruction in the interrupt path. */
static uint64_t get_cntv_ctl(const oemu_sysregs *sr) {
  const uint64_t stored = sr->cntv_ctl_el1 & ~(uint64_t)OEMU_GTIMER_CTL_ISTAT;
  const int pending =
      oemu_gtimer_pending(sr->cntvct - sr->cntvoff_el1, sr->cntv_ctl_el1, sr->cntv_cval_el1);
  return stored | ((pending != 0) ? (uint64_t)OEMU_GTIMER_CTL_ISTAT : 0U);
}
static uint64_t get_cntp_ctl(const oemu_sysregs *sr) {
  const uint64_t stored = sr->cntp_ctl_el1 & ~(uint64_t)OEMU_GTIMER_CTL_ISTAT;
  const int pending = oemu_gtimer_pending(sr->cntvct, sr->cntp_ctl_el1, sr->cntp_cval_el1);
  return stored | ((pending != 0) ? (uint64_t)OEMU_GTIMER_CTL_ISTAT : 0U);
}
static void set_cntv_ctl(oemu_sysregs *sr, uint64_t value) {
  sr->cntv_ctl_el1 = value & ~(uint64_t)OEMU_GTIMER_CTL_ISTAT;
}
static void set_cntp_ctl(oemu_sysregs *sr, uint64_t value) {
  sr->cntp_ctl_el1 = value & ~(uint64_t)OEMU_GTIMER_CTL_ISTAT;
}
/* TVAL is a *delta*: reading it returns comparator - counter, writing it sets
 * the comparator to counter + delta. Linux's arch timer clockevent programs
 * every tick through CNTV_TVAL_EL0 (not CNTV_CVAL_EL1), so leaving it
 * unimplemented dropped every re-arm on the floor: the comparator stayed in
 * the past, the PPI stayed pending, and the guest spent the whole boot
 * re-entering the timer handler instead of running. */
static uint64_t get_cntv_tval(const oemu_sysregs *sr) {
  return sr->cntv_cval_el1 - (sr->cntvct - sr->cntvoff_el1);
}
static void set_cntv_tval(oemu_sysregs *sr, uint64_t value) {
  sr->cntv_cval_el1 = (sr->cntvct - sr->cntvoff_el1) + value;
}
static void set_cntp_tval(oemu_sysregs *sr, uint64_t value) {
  sr->cntp_cval_el1 = sr->cntvct + value;
}

static const oemu_sysreg_row k_rows[] = {
    /* --- identification block: constant, EL0-readable ------------------------- */
    {.name = "MIDR_EL1",
     .sel = OEMU_SYSREG_MIDR_EL1,
     .min_el = OEMU_EL0,
     .flags = OEMU_SYSREG_F_RO,
     .offset = offsetof(oemu_sysregs, midr_el1),
     .write_mask = ~(uint64_t)0,
     .reset_value = OEMU_MIDR_EL1},
    {.name = "MPIDR_EL1",
     .sel = OEMU_SYSREG_MPIDR_EL1,
     .min_el = OEMU_EL0,
     .flags = OEMU_SYSREG_F_RO,
     .offset = offsetof(oemu_sysregs, mpidr_el1),
     .write_mask = ~(uint64_t)0,
     .reset_value = OEMU_MPIDR_EL1},
    {.name = "REVIDR_EL1",
     .sel = OEMU_SYSREG_REVIDR_EL1,
     .min_el = OEMU_EL0,
     .flags = OEMU_SYSREG_F_RO,
     .offset = offsetof(oemu_sysregs, revidr_el1),
     .write_mask = ~(uint64_t)0,
     .reset_value = OEMU_REVIDR_EL1},
    {.name = "MDSCR_EL1",
     .sel = OEMU_SYSREG_MDSCR_EL1,
     .min_el = OEMU_EL1,
     .flags = OEMU_SYSREG_F_WI,
     .offset = 0,
     .write_mask = 0,
     .reset_value = 0},
    {.name = "ID_AA64PFR0_EL1",
     .sel = OEMU_SYSREG_ID_AA64PFR0_EL1,
     .min_el = OEMU_EL0,
     .flags = OEMU_SYSREG_F_RO,
     .offset = offsetof(oemu_sysregs, id_aa64pfr0_el1),
     .write_mask = ~(uint64_t)0,
     .reset_value = OEMU_ID_AA64PFR0_EL1},
    /* AArch64 feature-ID registers the kernel reads at boot; modelled RAZ so an
     * optional-feature probe reads 0 (absent) rather than trapping. */
    {.name = "ID_AA64PFR1_EL1",
     .sel = OEMU_SYSREG_ID_AA64PFR1_EL1,
     .min_el = OEMU_EL0,
     .flags = OEMU_SYSREG_F_WI,
     .offset = 0,
     .write_mask = 0,
     .reset_value = 0},
    {.name = "ID_AA64PFR2_EL1",
     .sel = OEMU_SYSREG_ID_AA64PFR2_EL1,
     .min_el = OEMU_EL0,
     .flags = OEMU_SYSREG_F_WI,
     .offset = 0,
     .write_mask = 0,
     .reset_value = 0},
    {.name = "ID_AA64ZFR0_EL1",
     .sel = OEMU_SYSREG_ID_AA64ZFR0_EL1,
     .min_el = OEMU_EL0,
     .flags = OEMU_SYSREG_F_WI,
     .offset = 0,
     .write_mask = 0,
     .reset_value = 0},
    {.name = "ID_AA64SMFR0_EL1",
     .sel = OEMU_SYSREG_ID_AA64SMFR0_EL1,
     .min_el = OEMU_EL0,
     .flags = OEMU_SYSREG_F_WI,
     .offset = 0,
     .write_mask = 0,
     .reset_value = 0},
    {.name = "ID_AA64DFR0_EL1",
     .sel = OEMU_SYSREG_ID_AA64DFR0_EL1,
     .min_el = OEMU_EL0,
     .flags = OEMU_SYSREG_F_RO,
     .offset = offsetof(oemu_sysregs, id_aa64dfr0_el1),
     .write_mask = ~(uint64_t)0,
     .reset_value = OEMU_ID_AA64DFR0_EL1},
    {.name = "ID_AA64DFR1_EL1",
     .sel = OEMU_SYSREG_ID_AA64DFR1_EL1,
     .min_el = OEMU_EL0,
     .flags = OEMU_SYSREG_F_WI,
     .offset = 0,
     .write_mask = 0,
     .reset_value = 0},
    {.name = "ID_AA64DFR2_EL1",
     .sel = OEMU_SYSREG_ID_AA64DFR2_EL1,
     .min_el = OEMU_EL0,
     .flags = OEMU_SYSREG_F_WI,
     .offset = 0,
     .write_mask = 0,
     .reset_value = 0},
    {.name = "ID_AA64AFR0_EL1",
     .sel = OEMU_SYSREG_ID_AA64AFR0_EL1,
     .min_el = OEMU_EL0,
     .flags = OEMU_SYSREG_F_WI,
     .offset = 0,
     .write_mask = 0,
     .reset_value = 0},
    {.name = "ID_AA64ISAR0_EL1",
     .sel = OEMU_SYSREG_ID_AA64ISAR0_EL1,
     .min_el = OEMU_EL0,
     .flags = OEMU_SYSREG_F_RO,
     .offset = offsetof(oemu_sysregs, id_aa64isar0_el1),
     .write_mask = ~(uint64_t)0,
     .reset_value = OEMU_ID_AA64ISAR0_EL1},
    {.name = "ID_AA64ISAR1_EL1",
     .sel = OEMU_SYSREG_ID_AA64ISAR1_EL1,
     .min_el = OEMU_EL0,
     .flags = OEMU_SYSREG_F_RO,
     .offset = offsetof(oemu_sysregs, id_aa64isar1_el1),
     .write_mask = ~(uint64_t)0,
     .reset_value = OEMU_ID_AA64ISAR1_EL1},
    {.name = "ID_AA64ISAR2_EL1",
     .sel = OEMU_SYSREG_ID_AA64ISAR2_EL1,
     .min_el = OEMU_EL0,
     .flags = OEMU_SYSREG_F_WI,
     .offset = 0,
     .write_mask = 0,
     .reset_value = 0},
    {.name = "ID_AA64ISAR3_EL1",
     .sel = OEMU_SYSREG_ID_AA64ISAR3_EL1,
     .min_el = OEMU_EL0,
     .flags = OEMU_SYSREG_F_WI,
     .offset = 0,
     .write_mask = 0,
     .reset_value = 0},
    {.name = "ID_AA64ISAR4_EL1",
     .sel = OEMU_SYSREG_ID_AA64ISAR4_EL1,
     .min_el = OEMU_EL0,
     .flags = OEMU_SYSREG_F_WI,
     .offset = 0,
     .write_mask = 0,
     .reset_value = 0},
    {.name = "ID_AA64ISAR5_EL1",
     .sel = OEMU_SYSREG_ID_AA64ISAR5_EL1,
     .min_el = OEMU_EL0,
     .flags = OEMU_SYSREG_F_WI,
     .offset = 0,
     .write_mask = 0,
     .reset_value = 0},
    {.name = "ID_AA64MMFR0_EL1",
     .sel = OEMU_SYSREG_ID_AA64MMFR0_EL1,
     .min_el = OEMU_EL0,
     .flags = OEMU_SYSREG_F_RO,
     .offset = offsetof(oemu_sysregs, id_aa64mmfr0_el1),
     .write_mask = ~(uint64_t)0,
     .reset_value = OEMU_ID_AA64MMFR0_EL1},
    {.name = "ID_AA64MMFR1_EL1",
     .sel = OEMU_SYSREG_ID_AA64MMFR1_EL1,
     .min_el = OEMU_EL0,
     .flags = OEMU_SYSREG_F_RO,
     .offset = offsetof(oemu_sysregs, id_aa64mmfr1_el1),
     .write_mask = ~(uint64_t)0,
     .reset_value = OEMU_ID_AA64MMFR1_EL1},
    {.name = "ID_AA64MMFR2_EL1",
     .sel = OEMU_SYSREG_ID_AA64MMFR2_EL1,
     .min_el = OEMU_EL0,
     .flags = OEMU_SYSREG_F_WI,
     .offset = 0,
     .write_mask = 0,
     .reset_value = 0},
    {.name = "ID_AA64MMFR3_EL1",
     .sel = OEMU_SYSREG_ID_AA64MMFR3_EL1,
     .min_el = OEMU_EL0,
     .flags = OEMU_SYSREG_F_WI,
     .offset = 0,
     .write_mask = 0,
     .reset_value = 0},
    /* --- EL1 control registers (M3 stores, later honours) ---------------------- */
    {.name = "SCTLR_EL1",
     .sel = OEMU_SYSREG_SCTLR_EL1,
     .min_el = OEMU_EL1,
     .flags = OEMU_SYSREG_F_NONE,
     .offset = offsetof(oemu_sysregs, sctlr_el1),
     /* The ARM ARM's documented reset value; M3 interprets the MMU bits. */
     .write_mask = ~(uint64_t)0,
     .reset_value = 0x30D00800},
    {.name = "CPACR_EL1",
     .sel = OEMU_SYSREG_CPACR_EL1,
     .min_el = OEMU_EL1,
     .flags = OEMU_SYSREG_F_NONE,
     .offset = offsetof(oemu_sysregs, cpacr_el1),
     .write_mask = ~(uint64_t)0,
     .reset_value = 0},
    {.name = "OSLAR_EL1",
     .sel = OEMU_SYSREG_OSLAR_EL1,
     .min_el = OEMU_EL1,
     .flags = OEMU_SYSREG_F_WI,
     .offset = 0,
     .write_mask = 0,
     .reset_value = 0},
    {.name = "OSDLR_EL1",
     .sel = OEMU_SYSREG_OSDLR_EL1,
     .min_el = OEMU_EL1,
     .flags = OEMU_SYSREG_F_WI,
     .offset = 0,
     .write_mask = 0,
     .reset_value = 0},
    {.name = "TTBR0_EL1",
     .sel = OEMU_SYSREG_TTBR0_EL1,
     .min_el = OEMU_EL1,
     .flags = OEMU_SYSREG_F_NONE,
     .offset = offsetof(oemu_sysregs, ttbr0_el1),
     .write_mask = ~(uint64_t)0,
     .reset_value = 0},
    {.name = "TTBR1_EL1",
     .sel = OEMU_SYSREG_TTBR1_EL1,
     .min_el = OEMU_EL1,
     .flags = OEMU_SYSREG_F_NONE,
     .offset = offsetof(oemu_sysregs, ttbr1_el1),
     .write_mask = ~(uint64_t)0,
     .reset_value = 0},
    {.name = "TCR_EL1",
     .sel = OEMU_SYSREG_TCR_EL1,
     .min_el = OEMU_EL1,
     .flags = OEMU_SYSREG_F_NONE,
     .offset = offsetof(oemu_sysregs, tcr_el1),
     .write_mask = ~(uint64_t)0,
     .reset_value = 0},
    /* Control / debug / PMU-enable / PIE regs the early CPU-setup and resume
     * path writes. With the feature they belong to advertised as absent (the
     * ID regs above), the kernel writes a reset value and reads nothing back,
     * so a read-zero / write-ignored model is faithful and needs no backing
     * field. Without a row these MSR/MRS accesses trap Undefined and stop boot
     * inside __cpu_setup, before a single byte reaches the console. */
    {.name = "TCR2_EL1",
     .sel = OEMU_SYSREG_TCR2_EL1,
     .min_el = OEMU_EL1,
     .flags = OEMU_SYSREG_F_WI,
     .offset = 0,
     .write_mask = 0,
     .reset_value = 0},
    /* --- EL1 exception-link state, filled by the exc module (M2b) -------------- */
    {.name = "SPSR_EL1",
     .sel = OEMU_SYSREG_SPSR_EL1,
     .min_el = OEMU_EL1,
     .flags = OEMU_SYSREG_F_NONE,
     .offset = offsetof(oemu_sysregs, spsr_el[OEMU_EL1]),
     .write_mask = ~(uint64_t)0,
     .reset_value = 0},
    {.name = "ELR_EL1",
     .sel = OEMU_SYSREG_ELR_EL1,
     .min_el = OEMU_EL1,
     .flags = OEMU_SYSREG_F_NONE,
     .offset = offsetof(oemu_sysregs, elr_el[OEMU_EL1]),
     .write_mask = ~(uint64_t)0,
     .reset_value = 0},
    {.name = "SP_EL0",
     .sel = OEMU_SYSREG_SP_EL0,
     .min_el = OEMU_EL0,
     .flags = OEMU_SYSREG_F_NONE,
     .offset = 0,
     .write_mask = ~(uint64_t)0,
     .reset_value = 0,
     .get = get_sp_el0,
     .set = set_sp_el0},
    {.name = "SPSEL",
     .sel = OEMU_SYSREG_SPSEL,
     .min_el = OEMU_EL1,
     .flags = OEMU_SYSREG_F_NONE,
     .offset = 0,
     .write_mask = ~(uint64_t)0,
     .reset_value = 0,
     .get = get_sp_sel,
     .set = set_sp_sel},
    {.name = "CurrentEL",
     .sel = OEMU_SYSREG_CURRENT_EL,
     .min_el = OEMU_EL1,
     .flags = OEMU_SYSREG_F_RO,
     .offset = 0,
     .write_mask = ~(uint64_t)0,
     .reset_value = 0,
     .get = get_current_el},
    {.name = "ESR_EL1",
     .sel = OEMU_SYSREG_ESR_EL1,
     .min_el = OEMU_EL1,
     .flags = OEMU_SYSREG_F_NONE,
     .offset = offsetof(oemu_sysregs, esr_el[OEMU_EL1]),
     .write_mask = ~(uint64_t)0,
     .reset_value = 0},
    {.name = "FAR_EL1",
     .sel = OEMU_SYSREG_FAR_EL1,
     .min_el = OEMU_EL1,
     .flags = OEMU_SYSREG_F_NONE,
     .offset = offsetof(oemu_sysregs, far_el[OEMU_EL1]),
     .write_mask = ~(uint64_t)0,
     .reset_value = 0},
    {.name = "PAR_EL1",
     .sel = OEMU_SYSREG_PAR_EL1,
     .min_el = OEMU_EL1,
     .flags = OEMU_SYSREG_F_RO,
     .offset = offsetof(oemu_sysregs, par_el1),
     .write_mask = 0,
     .reset_value = 0},
    /* --- more EL1 control registers --------------------------------------------- */
    {.name = "MAIR_EL1",
     .sel = OEMU_SYSREG_MAIR_EL1,
     .min_el = OEMU_EL1,
     .flags = OEMU_SYSREG_F_NONE,
     .offset = offsetof(oemu_sysregs, mair_el1),
     .write_mask = ~(uint64_t)0,
     .reset_value = 0},
    {.name = "PIRE0_EL1",
     .sel = OEMU_SYSREG_PIRE0_EL1,
     .min_el = OEMU_EL1,
     .flags = OEMU_SYSREG_F_WI,
     .offset = 0,
     .write_mask = 0,
     .reset_value = 0},
    {.name = "PIR_EL1",
     .sel = OEMU_SYSREG_PIR_EL1,
     .min_el = OEMU_EL1,
     .flags = OEMU_SYSREG_F_WI,
     .offset = 0,
     .write_mask = 0,
     .reset_value = 0},
    {.name = "AMAIR_EL1",
     .sel = OEMU_SYSREG_AMAIR_EL1,
     .min_el = OEMU_EL1,
     .flags = OEMU_SYSREG_F_NONE,
     .offset = offsetof(oemu_sysregs, amair_el1),
     .write_mask = ~(uint64_t)0,
     .reset_value = 0},
    {.name = "VBAR_EL1",
     .sel = OEMU_SYSREG_VBAR_EL1,
     .min_el = OEMU_EL1,
     .flags = OEMU_SYSREG_F_NONE,
     .offset = offsetof(oemu_sysregs, vbar_el[OEMU_EL1]),
     .write_mask = ~(uint64_t)0,
     .reset_value = 0},
    {.name = "DISR_EL1",
     .sel = OEMU_SYSREG_DISR_EL1,
     .min_el = OEMU_EL1,
     .flags = OEMU_SYSREG_F_WI,
     .offset = 0,
     .write_mask = 0,
     .reset_value = 0},
    {.name = "CONTEXTIDR_EL1",
     .sel = OEMU_SYSREG_CONTEXTIDR_EL1,
     .min_el = OEMU_EL1,
     .flags = OEMU_SYSREG_F_NONE,
     .offset = offsetof(oemu_sysregs, contextidr_el1),
     .write_mask = ~(uint64_t)0,
     .reset_value = 0},
    {.name = "TPIDR_EL1",
     .sel = OEMU_SYSREG_TPIDR_EL1,
     .min_el = OEMU_EL1,
     .flags = OEMU_SYSREG_F_NONE,
     .offset = offsetof(oemu_sysregs, tpidr_el1),
     .write_mask = ~(uint64_t)0,
     .reset_value = 0},
    {.name = "CNTKCTL_EL1",
     .sel = OEMU_SYSREG_CNTKCTL_EL1,
     .min_el = OEMU_EL1,
     .flags = 0,
     .offset = offsetof(oemu_sysregs, cntkctl_el1),
     .write_mask = ~(uint64_t)0,
     .reset_value = 0},
    /* --- cache identification: constants, EL1 ---------------------------------- */
    {.name = "CCSIDR_EL1",
     .sel = OEMU_SYSREG_CCSIDR_EL1,
     .min_el = OEMU_EL1,
     .flags = OEMU_SYSREG_F_RO,
     .offset = offsetof(oemu_sysregs, ccsidr_el1),
     .write_mask = ~(uint64_t)0,
     .reset_value = OEMU_CCSIDR_EL1},
    {.name = "CLIDR_EL1",
     .sel = OEMU_SYSREG_CLIDR_EL1,
     .min_el = OEMU_EL1,
     .flags = OEMU_SYSREG_F_RO,
     .offset = offsetof(oemu_sysregs, clidr_el1),
     .write_mask = ~(uint64_t)0,
     .reset_value = OEMU_CLIDR_EL1},
    /* CSSELR would switch the CCSIDR view; with one cache format modelled it
     * is RAZ/WI rather than a lie about selectable formats. */
    {.name = "CSSELR_EL1",
     .sel = OEMU_SYSREG_CSSELR_EL1,
     .min_el = OEMU_EL1,
     .flags = OEMU_SYSREG_F_WI,
     .offset = 0,
     .write_mask = ~(uint64_t)0,
     .reset_value = 0},
    /* --- EL0-readable instruction-set identification --------------------------- */
    {.name = "CTR_EL0",
     .sel = OEMU_SYSREG_CTR_EL0,
     .min_el = OEMU_EL0,
     .flags = OEMU_SYSREG_F_RO,
     .offset = offsetof(oemu_sysregs, ctr_el0),
     .write_mask = ~(uint64_t)0,
     .reset_value = OEMU_CTR_EL0},
    {.name = "DCZID_EL0",
     .sel = OEMU_SYSREG_DCZID_EL0,
     .min_el = OEMU_EL0,
     .flags = OEMU_SYSREG_F_RO,
     .offset = offsetof(oemu_sysregs, dczid_el0),
     .write_mask = ~(uint64_t)0,
     .reset_value = OEMU_DCZID_EL0},
    {.name = "NZCV",
     .sel = OEMU_SYSREG_NZCV,
     .min_el = OEMU_EL0,
     .flags = OEMU_SYSREG_F_NONE,
     .offset = 0,
     .write_mask = ~(uint64_t)0,
     .reset_value = 0,
     .get = get_nzcv,
     .set = set_nzcv},
    {.name = "DAIF",
     .sel = OEMU_SYSREG_DAIF,
     .min_el = OEMU_EL1,
     .flags = OEMU_SYSREG_F_NONE,
     .offset = 0,
     .write_mask = ~(uint64_t)0,
     .reset_value = 0,
     .get = get_daif,
     .set = set_daif},
    {.name = "FPCR",
     .sel = OEMU_SYSREG_FPCR,
     .min_el = OEMU_EL0,
     .flags = OEMU_SYSREG_F_NONE,
     .offset = offsetof(oemu_sysregs, fpcr),
     .write_mask = UINT64_C(0xFFFFFFFF),
     .reset_value = 0},
    {.name = "FPSR",
     .sel = OEMU_SYSREG_FPSR,
     .min_el = OEMU_EL0,
     .flags = OEMU_SYSREG_F_NONE,
     .offset = offsetof(oemu_sysregs, fpsr),
     .write_mask = UINT64_C(0xFFFFFFFF),
     .reset_value = 0},
    {.name = "PMUSERENR_EL0",
     .sel = OEMU_SYSREG_PMUSERENR_EL0,
     .min_el = OEMU_EL0,
     .flags = OEMU_SYSREG_F_WI,
     .offset = 0,
     .write_mask = 0,
     .reset_value = 0},
    /* The M1 facade keeps its own tpidrur_el0 on oemu_cpu; in system mode the
     * table owns user thread state, so the legacy field is unused here. */
    {.name = "TPIDR_EL0",
     .sel = OEMU_SYSREG_TPIDR_EL0,
     .min_el = OEMU_EL0,
     .flags = OEMU_SYSREG_F_NONE,
     .offset = offsetof(oemu_sysregs, tpidr_el0),
     .write_mask = ~(uint64_t)0,
     .reset_value = 0},
    {.name = "TPIDRRO_EL0",
     .sel = OEMU_SYSREG_TPIDRRO_EL0,
     .min_el = OEMU_EL0,
     .flags = OEMU_SYSREG_F_NONE,
     .offset = offsetof(oemu_sysregs, tpidrro_el0),
     .write_mask = ~(uint64_t)0,
     .reset_value = 0},
    {.name = "AMUSERENR_EL0",
     .sel = OEMU_SYSREG_AMUSERENR_EL0,
     .min_el = OEMU_EL0,
     .flags = OEMU_SYSREG_F_WI,
     .offset = 0,
     .write_mask = 0,
     .reset_value = 0},
    /* --- generic timer (M4b, minimal): the kernel reads CNTVCT_EL0 and the
     * frequency at boot (__delay_cycles, the clock source) and the clock event
     * device programs CNTp_CTL/CVAL. A counter that advances with guest
     * progress is what lets a busy-wait delay terminate; delivering the timer
     * IRQ itself is the rest of M4b. */
    {.name = "CNTFRQ_EL0",
     .sel = OEMU_SYSREG_CNTFRQ_EL0,
     .min_el = OEMU_EL0,
     .flags = OEMU_SYSREG_F_RO,
     .offset = offsetof(oemu_sysregs, cntfrq_el0),
     .write_mask = ~(uint64_t)0,
     .reset_value = OEMU_CNTFRQ_EL0_DEFAULT},
    {.name = "CNTPCT_EL0",
     .sel = OEMU_SYSREG_CNTPCT_EL0,
     .min_el = OEMU_EL0,
     .flags = OEMU_SYSREG_F_RO,
     .offset = 0,
     .write_mask = 0,
     .reset_value = 0,
     .get = get_cntpct},
    {.name = "CNTVCT_EL0",
     .sel = OEMU_SYSREG_CNTVCT_EL0,
     .min_el = OEMU_EL0,
     .flags = OEMU_SYSREG_F_RO,
     .offset = 0,
     .write_mask = 0,
     .reset_value = 0,
     .get = get_cntvct},
    /* Remaining generic-timer bank the arch timer reaches. The guest keys
     * these by the 14-bit selector it emits, which folds op0 away, so the
     * privileged CNTP_* / CNTVOFF views land in the same 0x1f__ band as the
     * EL0 counter views (CNTKCTL_EL1 alone keeps its low 0x07__ slot). Values
     * are read straight out of vmlinux, never from an alias table. */
    /* FPCR/FPSR: the two FP status registers, and they are not optional for a
     * modern Linux. 6.6's fpsimd_load_state/fpsimd_save_state run on every
     * return to user mode and issue `mrs x0, fpcr` / `msr fpcr, x8`
     * unconditionally -- system_supports_fpsimd() is !have_cpucap(ARM64_HAS_
     * NO_FPSIMD), and that cap is a dummy nothing can set -- so refusing these
     * selectors traps an Undefined instruction inside the return-to-user path
     * and the guest dies before /init ever starts. Both are EL0-accessible,
     * both are 32-bit wide (the write mask is the architecture's, not ours),
     * and both reset to zero, which is what the oracle's Cortex-A53 shows. */
    {.name = "CNTVOFF_EL1",
     .sel = OEMU_SYSREG_CNTVOFF_EL1,
     .min_el = OEMU_EL1,
     .flags = 0,
     .offset = offsetof(oemu_sysregs, cntvoff_el1),
     .write_mask = ~(uint64_t)0,
     .reset_value = 0},
    {.name = "CNTPCTSS_EL0",
     .sel = OEMU_SYSREG_CNTPCTSS_EL0,
     .min_el = OEMU_EL1,
     .flags = OEMU_SYSREG_F_RO,
     .offset = 0,
     .write_mask = 0,
     .reset_value = 0,
     .get = get_cntpct},
    {.name = "CNTVCTSS_EL0",
     .sel = OEMU_SYSREG_CNTVCTSS_EL0,
     .min_el = OEMU_EL1,
     .flags = OEMU_SYSREG_F_RO,
     .offset = 0,
     .write_mask = 0,
     .reset_value = 0,
     .get = get_cntvct},
    {.name = "CNTP_CTL_EL1",
     .sel = OEMU_SYSREG_CNTP_CTL_EL1,
     .min_el = OEMU_EL1,
     .flags = 0,
     .offset = 0,
     .write_mask = ~(uint64_t)0,
     .reset_value = 0,
     .get = get_cntp_ctl,
     .set = set_cntp_ctl},
    {.name = "CNTP_CVAL_EL1",
     .sel = OEMU_SYSREG_CNTP_CVAL_EL1,
     .min_el = OEMU_EL1,
     .flags = 0,
     .offset = offsetof(oemu_sysregs, cntp_cval_el1),
     .write_mask = ~(uint64_t)0,
     .reset_value = 0},
    {.name = "CNTP_TVAL_EL1",
     .sel = OEMU_SYSREG_CNTP_TVAL_EL1,
     .min_el = OEMU_EL1,
     .flags = 0,
     .offset = 0,
     .write_mask = ~(uint64_t)0,
     .reset_value = 0,
     .get = get_cntp_tval,
     .set = set_cntp_tval},
    {.name = "CNTV_CTL_EL0",
     .sel = OEMU_SYSREG_CNTV_CTL_EL0,
     .min_el = OEMU_EL1,
     .flags = 0,
     .offset = 0,
     .write_mask = ~(uint64_t)0,
     .reset_value = 0,
     .get = get_cntv_ctl,
     .set = set_cntv_ctl},
    {.name = "CNTV_CVAL_EL0",
     .sel = OEMU_SYSREG_CNTV_CVAL_EL0,
     .min_el = OEMU_EL1,
     .flags = 0,
     .offset = offsetof(oemu_sysregs, cntv_cval_el1),
     .write_mask = ~(uint64_t)0,
     .reset_value = 0},
    {.name = "CNTV_TVAL_EL0",
     .sel = OEMU_SYSREG_CNTV_TVAL_EL0,
     .min_el = OEMU_EL1,
     .flags = 0,
     .offset = 0,
     .write_mask = ~(uint64_t)0,
     .reset_value = 0,
     .get = get_cntv_tval,
     .set = set_cntv_tval},
    /* SP_ELx sits in the op1 bank of the level ABOVE it (op1=4 = EL2+), so
     * SP_EL1 is reachable from EL2 and EL3 only; an EL1 guest uses its own
     * banked SP through SP plus SPSel instead. min_el=OEMU_EL2 here means
     * "requires EL2 or above", reachable in practice only from EL3. The
     * callbacks keep the single-bank model: the active bank's live copy is
     * regs->sp, so table writes must move it too. */
    {.name = "SP_EL1",
     .sel = OEMU_SYSREG_SP_EL1,
     .min_el = OEMU_EL2,
     .flags = OEMU_SYSREG_F_NONE,
     .offset = 0,
     .write_mask = ~(uint64_t)0,
     .reset_value = 0,
     .get = get_sp_el1,
     .set = set_sp_el1},
    /* --- EL3 exception-link state (exception entry and ERET only) -------------- */
    {.name = "SPSR_EL3",
     .sel = OEMU_SYSREG_SPSR_EL3,
     .min_el = OEMU_EL3,
     .flags = OEMU_SYSREG_F_NONE,
     .offset = offsetof(oemu_sysregs, spsr_el[OEMU_EL3]),
     .write_mask = ~(uint64_t)0,
     .reset_value = 0},
    {.name = "ELR_EL3",
     .sel = OEMU_SYSREG_ELR_EL3,
     .min_el = OEMU_EL3,
     .flags = OEMU_SYSREG_F_NONE,
     .offset = offsetof(oemu_sysregs, elr_el[OEMU_EL3]),
     .write_mask = ~(uint64_t)0,
     .reset_value = 0},
    {.name = "ESR_EL3",
     .sel = OEMU_SYSREG_ESR_EL3,
     .min_el = OEMU_EL3,
     .flags = OEMU_SYSREG_F_NONE,
     .offset = offsetof(oemu_sysregs, esr_el[OEMU_EL3]),
     .write_mask = ~(uint64_t)0,
     .reset_value = 0},
    {.name = "FAR_EL3",
     .sel = OEMU_SYSREG_FAR_EL3,
     .min_el = OEMU_EL3,
     .flags = OEMU_SYSREG_F_NONE,
     .offset = offsetof(oemu_sysregs, far_el[OEMU_EL3]),
     .write_mask = ~(uint64_t)0,
     .reset_value = 0},
    {.name = "VBAR_EL3",
     .sel = OEMU_SYSREG_VBAR_EL3,
     .min_el = OEMU_EL3,
     .flags = OEMU_SYSREG_F_NONE,
     .offset = offsetof(oemu_sysregs, vbar_el[OEMU_EL3]),
     .write_mask = ~(uint64_t)0,
     .reset_value = 0},

};

#define OEMU_SYSREG_ROW_COUNT (sizeof(k_rows) / sizeof(k_rows[0]))

oemu_sysreg_table oemu_sysreg_internal_table(void) {
  const oemu_sysreg_table table = {k_rows, OEMU_SYSREG_ROW_COUNT};
  return table;
}

const oemu_sysreg_row *oemu_sysreg_internal_find(uint32_t sel) {
  for (size_t i = 0; i < OEMU_SYSREG_ROW_COUNT; i++) {
    if (k_rows[i].sel == sel) {
      return &k_rows[i];
    }
  }
  return NULL;
}

/* --- public API ------------------------------------------------------------------ */

void oemu_sysregs_init(oemu_sysregs *sr, oemu_regs *regs, oemu_el el) {
  OEMU_REQUIRE(sr != NULL, "NULL oemu_sysregs");
  OEMU_REQUIRE(regs != NULL, "NULL oemu_regs for oemu_sysregs");
  OEMU_REQUIRE(el != OEMU_EL2, "oemu implements no EL2");

  /* Zero first so slots with no table row (sp_el[0], sp_el[2], the EL0 bank
   * slots) are deterministic rather than caller-dependent. */
  *sr = (oemu_sysregs){0};
  sr->regs = regs;

  const oemu_sysreg_table table = oemu_sysreg_internal_table();
  for (size_t i = 0; i < table.count; i++) {
    const oemu_sysreg_row *row = &table.rows[i];
    if (row->offset == 0) {
      /* Callback and WI rows keep no storage here; their backing is regs,
       * pstate, or nothing, and none has a meaningful reset in this struct. */
      continue;
    }
    /* memcpy rather than a pointer cast: -Wcast-align forbids widening the
     * alignment through a char* offset, and bugprone-casting-through-void
     * forbids the void* hop that would dodge it. Compilers fold the fixed
     * 8-byte copy into a direct load/store. */
    memcpy((char *)sr + row->offset, &row->reset_value, sizeof(row->reset_value));
  }

  /* Boot PSTATE: target mode (h-form above EL0, so SPSel=1 as the DT booting
   * convention requires), interrupts masked. IL stays clear because IL=1
   * would make the boot state itself architecturally illegal. */
  sr->pstate = oemu_pstate_mode(el) | (OEMU_PSTATE_DAIF_MASK << OEMU_PSTATE_DAIF_SHIFT);

  /* Seed the bank that holds the initial stack pointer (oemu/exc.h's
   * mirroring invariant): the boot bank gets regs->sp, the rest stay 0 so a
   * bank switch away from an unseeded bank reads 0 rather than the caller's
   * stack garbage. */
  sr->sp_el[(el == OEMU_EL0) ? 0 : el] = regs->sp;
}

static bool row_access_allowed(const oemu_sysregs *sr, const oemu_sysreg_row *row) {
  return oemu_pstate_el(sr->pstate) >= (oemu_el)row->min_el;
}

static uint64_t row_read(const oemu_sysregs *sr, const oemu_sysreg_row *row) {
  if (row->get != NULL) {
    return row->get(sr);
  }
  if ((row->flags & OEMU_SYSREG_F_WI) != 0) {
    return 0;
  }
  /* memcpy for the same cast rules as oemu_sysregs_init above. */
  uint64_t raw = 0;
  memcpy(&raw, (const char *)sr + row->offset, sizeof(raw));
  return raw & row->write_mask;
}

static void row_write(oemu_sysregs *sr, const oemu_sysreg_row *row, uint64_t value) {
  if (row->set != NULL) {
    row->set(sr, value);
    return;
  }
  uint64_t current = 0;
  memcpy(&current, (char *)sr + row->offset, sizeof(current));
  current = (current & ~row->write_mask) | (value & row->write_mask);
  memcpy((char *)sr + row->offset, &current, sizeof(current));
}

OEMU_NODISCARD oemu_status oemu_sysreg_read(const oemu_sysregs *sr, uint32_t sel,
                                            uint64_t *out) {
  OEMU_REQUIRE(sr != NULL, "NULL oemu_sysregs");
  OEMU_REQUIRE(out != NULL, "NULL out for oemu_sysreg_read");

  const oemu_sysreg_row *row = oemu_sysreg_internal_find(sel);
  if (row == NULL || !row_access_allowed(sr, row)) {
    /* Unimplemented encoding and too-low-EL both trap Undefined on real
     * hardware, so the caller gets one signal for both. */
    return OEMU_ERR_UNSUPPORTED;
  }
  *out = row_read(sr, row);
  return OEMU_OK;
}

OEMU_NODISCARD oemu_status oemu_sysreg_write(oemu_sysregs *sr, uint32_t sel, uint64_t value) {
  OEMU_REQUIRE(sr != NULL, "NULL oemu_sysregs");

  const oemu_sysreg_row *row = oemu_sysreg_internal_find(sel);
  if (row == NULL || !row_access_allowed(sr, row) || (row->flags & OEMU_SYSREG_F_RO) != 0) {
    return OEMU_ERR_UNSUPPORTED;
  }
  if ((row->flags & OEMU_SYSREG_F_WI) != 0) {
    /* RAZ/WI: the write succeeds and changes nothing. */
    return OEMU_OK;
  }
  row_write(sr, row, value);
  return OEMU_OK;
}

const char *oemu_sysreg_name(uint32_t sel) {
  const oemu_sysreg_row *row = oemu_sysreg_internal_find(sel);
  return row != NULL ? row->name : "unknown";
}
