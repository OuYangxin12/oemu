/*
 * System-register table and the MRS/MSR accessors built on it. See
 * oemu/sysreg.h for the state layout and the trap-vs-status contract.
 */
#include "oemu/sysreg.h"

#include "oemu/check.h"
#include "oemu/macros.h"
#include "oemu/regs.h"
#include "oemu/status.h"

#include <stddef.h>
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
  sr->pstate = (sr->pstate & ~OEMU_PSTATE_SPSEL) | (value & OEMU_PSTATE_SPSEL);
}

static uint64_t get_daif(const oemu_sysregs *sr) {
  return (sr->pstate >> OEMU_PSTATE_DAIF_SHIFT) & OEMU_PSTATE_DAIF_MASK;
}

static void set_daif(oemu_sysregs *sr, uint64_t value) {
  const uint64_t field = OEMU_PSTATE_DAIF_MASK << OEMU_PSTATE_DAIF_SHIFT;
  sr->pstate =
      (sr->pstate & ~field) | ((value & OEMU_PSTATE_DAIF_MASK) << OEMU_PSTATE_DAIF_SHIFT);
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

static uint64_t get_sp_el0(const oemu_sysregs *sr) {
  return oemu_regs_sp(sr->regs);
}

static void set_sp_el0(oemu_sysregs *sr, uint64_t value) {
  oemu_regs_set_sp(sr->regs, value);
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
    {.name = "ID_AA64PFR0_EL1",
     .sel = OEMU_SYSREG_ID_AA64PFR0_EL1,
     .min_el = OEMU_EL0,
     .flags = OEMU_SYSREG_F_RO,
     .offset = offsetof(oemu_sysregs, id_aa64pfr0_el1),
     .write_mask = ~(uint64_t)0,
     .reset_value = OEMU_ID_AA64PFR0_EL1},
    {.name = "ID_AA64DFR0_EL1",
     .sel = OEMU_SYSREG_ID_AA64DFR0_EL1,
     .min_el = OEMU_EL0,
     .flags = OEMU_SYSREG_F_RO,
     .offset = offsetof(oemu_sysregs, id_aa64dfr0_el1),
     .write_mask = ~(uint64_t)0,
     .reset_value = OEMU_ID_AA64DFR0_EL1},
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

    /* --- more EL1 control registers --------------------------------------------- */
    {.name = "MAIR_EL1",
     .sel = OEMU_SYSREG_MAIR_EL1,
     .min_el = OEMU_EL1,
     .flags = OEMU_SYSREG_F_NONE,
     .offset = offsetof(oemu_sysregs, mair_el1),
     .write_mask = ~(uint64_t)0,
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

    /* --- thread IDs ------------------------------------------------------------- */
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
     .flags = OEMU_SYSREG_F_RO,
     .offset = offsetof(oemu_sysregs, tpidrro_el0),
     .write_mask = ~(uint64_t)0,
     .reset_value = 0},

    /* --- banked stack pointers -------------------------------------------------- */
    /* SP_ELx sits in the op1 bank of the level ABOVE it (op1=4 = EL2+), so
     * SP_EL1 is reachable from EL2 and EL3 only; an EL1 guest uses its own
     * banked SP through SP plus SPSel instead. min_el=OEMU_EL2 here means
     * "requires EL2 or above", reachable in practice only from EL3. */
    {.name = "SP_EL1",
     .sel = OEMU_SYSREG_SP_EL1,
     .min_el = OEMU_EL2,
     .flags = OEMU_SYSREG_F_NONE,
     .offset = offsetof(oemu_sysregs, sp_el[OEMU_EL1]),
     .write_mask = ~(uint64_t)0,
     .reset_value = 0},

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

  /* Boot PSTATE: target mode, interrupts masked, SPSel=1 from EL1 up (the DT
   * booting convention for a kernel entered at EL1). IL stays clear because
   * IL=1 would make the boot state itself architecturally illegal. */
  sr->pstate = oemu_pstate_mode(el) | (OEMU_PSTATE_DAIF_MASK << OEMU_PSTATE_DAIF_SHIFT);
  if (el != OEMU_EL0) {
    sr->pstate |= OEMU_PSTATE_SPSEL;
  }
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
