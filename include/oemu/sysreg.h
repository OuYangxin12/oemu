/*
 * AArch64 system-register state for the system-mode CPU (M2).
 *
 * The struct is embedded by value in the vCPU (roadmap: no allocation on the
 * boot or step paths) and pairs one `oemu_regs` register file with everything
 * that register file cannot hold: the banked exception-link registers, the
 * EL1 control/translation registers and the constant identification block.
 * Handle it by pointer and never copy it: the pairing with `oemu_regs` is
 * identity, not data.
 *
 * Two pieces of state deliberately live in `oemu_regs` rather than here:
 *
 *   - SP_EL0 in `oemu_regs.sp`, and NZCV in `oemu_regs.nzcv`, because the EL0
 *     interpreter already owns them and duplicating them would need a
 *     write-through discipline everywhere. The table below routes the SP_EL0
 *     and NZCV encodings at those fields through callbacks.
 *
 *   - PC, because exceptions swap it under ESR/ELR discipline in the exc
 *     module (M2b), not through a system-register encoding.
 *
 * Access is table-driven: `oemu_sysreg_read` / `oemu_sysreg_write` look an
 * encoding up in one table that carries the register's name, its minimum
 * accessible exception level, its reset value and (for plain fields) its
 * storage offset. That one table replaces the hardcoded MRS/MSR whitelist the
 * EL0 interpreter used, and it is what the white-box tests validate wholesale.
 *
 * Any access the architecture would trap at -- an unimplemented encoding, a
 * read-only register, or a register above the current exception level --
 * returns OEMU_ERR_UNSUPPORTED. That is the caller's signal to inject
 * Undefined (EC 0x18) in system mode; the EL0 facade keeps its own
 * error-return contract and never reaches this table.
 *
 * Deferred on purpose, each to the milestone that can test it: the generic
 * timer (CNTFRQ/CNTVCT, M4 with the DT), the GIC ICC_* bank (M4), cache and
 * TLB maintenance plus the MMU control semantics of SCTLR/TTBR/TCR/MAIR (M3
 * stores them but does not yet honour them), MDSCR_EL1 (required before a
 * Linux guest, which writes it during early boot), and FP/SIMD state --
 * FPCR/FPSR have no row on purpose, so FP access reads as Undefined, matching
 * ID_AA64PFR0 advertising no FP/SIMD (roadmap D7).
 */
#ifndef OEMU_SYSREG_H
#define OEMU_SYSREG_H

#include "oemu/macros.h"
#include "oemu/regs.h"
#include "oemu/status.h"

#include <stdbool.h>
#include <stdint.h>

OEMU_BEGIN_DECLS

/* --- exception levels --------------------------------------------------------- */

typedef enum oemu_el {
  OEMU_EL0 = 0,
  OEMU_EL1 = 1,
  /* Encoded so tables can be indexed by level, but oemu implements no EL2:
   * exceptions never target it and no register row gates on it (roadmap D2). */
  OEMU_EL2 = 2,
  OEMU_EL3 = 3
} oemu_el;

#define OEMU_EL_COUNT ((unsigned)4)

/* --- PSTATE -------------------------------------------------------------------- */
/*
 * Only the bits oemu models. The mode field is bits [4:0]: level in bits
 * [3:2], the SP form in bit 0 (h), and bit 4 clear for AArch64 (set only for
 * AArch32 modes, which oemu does not implement).
 */
#define OEMU_PSTATE_M_MASK ((uint64_t)0x1F)
#define OEMU_PSTATE_M_EL0T ((uint64_t)0x00)
#define OEMU_PSTATE_M_EL1H ((uint64_t)0x05)
#define OEMU_PSTATE_M_EL2H ((uint64_t)0x09)
#define OEMU_PSTATE_M_EL3H ((uint64_t)0x0D)

/* Bit 0: which SP the guest uses -- 1 = SP_ELx of the current level, 0 = SP_EL0. */
#define OEMU_PSTATE_SPSEL ((uint64_t)0x01)
/* Bits 9:6 = D/A/I/F interrupt masks, 1 = masked. Stored in PSTATE layout. */
#define OEMU_PSTATE_DAIF_SHIFT ((unsigned)6)
#define OEMU_PSTATE_DAIF_MASK  ((uint64_t)0xF)
/* Bit 20: illegal-execution-state flag. Never set by oemu itself: an IL=1
 * PSTATE is architecturally illegal, so boot and exception entry keep it 0. */
#define OEMU_PSTATE_IL ((uint64_t)0x100000)
/* Bit 21: software-step active. Cleared until stepping is modelled (M5). */
#define OEMU_PSTATE_SS ((uint64_t)0x200000)

/* The PSTATE mode value for running at `el`: EL0 uses the t form (no SP
 * choice), every other level the h form. */
static inline uint64_t oemu_pstate_mode(oemu_el el) {
  switch (el) {
    case OEMU_EL0:
      return OEMU_PSTATE_M_EL0T;
    case OEMU_EL1:
      return OEMU_PSTATE_M_EL1H;
    case OEMU_EL2:
      return OEMU_PSTATE_M_EL2H;
    case OEMU_EL3:
      return OEMU_PSTATE_M_EL3H;
    default:
      return OEMU_PSTATE_M_EL1H;
  }
}

/* Current exception level extracted from a PSTATE value. */
static inline oemu_el oemu_pstate_el(uint64_t pstate) {
  return (oemu_el)((pstate >> 2) & 3);
}

static inline unsigned oemu_pstate_sp_sel(uint64_t pstate) {
  return (unsigned)(pstate & OEMU_PSTATE_SPSEL);
}

/* The four interrupt masks, in DAIF order (D is the most significant bit). */
static inline unsigned oemu_pstate_daif(uint64_t pstate) {
  return (unsigned)((pstate >> OEMU_PSTATE_DAIF_SHIFT) & OEMU_PSTATE_DAIF_MASK);
}

/* --- system-register encodings ------------------------------------------------- */
/*
 * The 14-bit operand of MRS/MSR, extracted as (insn >> 5) & 0x3FFF. That mask
 * erases instruction bit 19 -- the MRS/MSR direction bit -- so one constant
 * serves both directions of a register. Values were harvested from clang's
 * integrated AArch64 assembler; build/sysgen/encodings.txt keeps the raw
 * table and the white-box test pins the sensitive ones.
 */
#define OEMU_SYSREG_MIDR_EL1         ((uint32_t)0x0000)
#define OEMU_SYSREG_MPIDR_EL1        ((uint32_t)0x0005)
#define OEMU_SYSREG_REVIDR_EL1       ((uint32_t)0x0006)
#define OEMU_SYSREG_ID_AA64PFR0_EL1  ((uint32_t)0x0020)
#define OEMU_SYSREG_ID_AA64DFR0_EL1  ((uint32_t)0x0028)
#define OEMU_SYSREG_ID_AA64ISAR0_EL1 ((uint32_t)0x0030)
#define OEMU_SYSREG_ID_AA64ISAR1_EL1 ((uint32_t)0x0031)
#define OEMU_SYSREG_ID_AA64MMFR0_EL1 ((uint32_t)0x0038)
#define OEMU_SYSREG_ID_AA64MMFR1_EL1 ((uint32_t)0x0039)
#define OEMU_SYSREG_SCTLR_EL1        ((uint32_t)0x0080)
#define OEMU_SYSREG_CPACR_EL1        ((uint32_t)0x0082)
#define OEMU_SYSREG_TTBR0_EL1        ((uint32_t)0x0100)
#define OEMU_SYSREG_TTBR1_EL1        ((uint32_t)0x0101)
#define OEMU_SYSREG_TCR_EL1          ((uint32_t)0x0102)
#define OEMU_SYSREG_SPSR_EL1         ((uint32_t)0x0200)
#define OEMU_SYSREG_ELR_EL1          ((uint32_t)0x0201)
#define OEMU_SYSREG_SP_EL0           ((uint32_t)0x0208)
#define OEMU_SYSREG_SP_EL1           ((uint32_t)0x2208)
#define OEMU_SYSREG_SPSEL            ((uint32_t)0x0210)
#define OEMU_SYSREG_CURRENT_EL       ((uint32_t)0x0212)
#define OEMU_SYSREG_ESR_EL1          ((uint32_t)0x0290)
#define OEMU_SYSREG_FAR_EL1          ((uint32_t)0x0300)
#define OEMU_SYSREG_MAIR_EL1         ((uint32_t)0x0510)
#define OEMU_SYSREG_AMAIR_EL1        ((uint32_t)0x0518)
#define OEMU_SYSREG_VBAR_EL1         ((uint32_t)0x0600)
#define OEMU_SYSREG_CONTEXTIDR_EL1   ((uint32_t)0x0681)
#define OEMU_SYSREG_TPIDR_EL1        ((uint32_t)0x0684)
#define OEMU_SYSREG_CCSIDR_EL1       ((uint32_t)0x0800)
#define OEMU_SYSREG_CLIDR_EL1        ((uint32_t)0x0801)
#define OEMU_SYSREG_CSSELR_EL1       ((uint32_t)0x1000)
#define OEMU_SYSREG_CTR_EL0          ((uint32_t)0x1801)
#define OEMU_SYSREG_DCZID_EL0        ((uint32_t)0x1807)
#define OEMU_SYSREG_NZCV             ((uint32_t)0x1A10)
#define OEMU_SYSREG_DAIF             ((uint32_t)0x1A11)
#define OEMU_SYSREG_TPIDR_EL0        ((uint32_t)0x1E82)
#define OEMU_SYSREG_TPIDRRO_EL0      ((uint32_t)0x1E83)
#define OEMU_SYSREG_SPSR_EL3         ((uint32_t)0x3200)
#define OEMU_SYSREG_ELR_EL3          ((uint32_t)0x3201)
#define OEMU_SYSREG_ESR_EL3          ((uint32_t)0x3290)
#define OEMU_SYSREG_FAR_EL3          ((uint32_t)0x3300)
#define OEMU_SYSREG_VBAR_EL3         ((uint32_t)0x3600)

/*
 * Constant identification-block values, chosen to describe exactly what oemu
 * implements. Each is the honest advertisement, not a copy of some board:
 * a guest that believes a feature exists will execute its instructions and
 * fault somewhere confusing, so anything oemu does not implement reads as
 * not-implemented rather than present.
 */
/* AArch64-only EL0/EL1/EL3, no EL2, no GIC, FP and AdvSIMD not implemented. */
#define OEMU_ID_AA64PFR0_EL1 ((uint64_t)0x00FF1F11)
/* No cryptographic, atomic (LSE), CRC32 or RDM instructions. */
#define OEMU_ID_AA64ISAR0_EL1 ((uint64_t)0x00000000)
/* No pointer authentication, JSCVT, FCMA or LRCPC. */
#define OEMU_ID_AA64ISAR1_EL1 ((uint64_t)0x00000000)
/* ARMv8 debug architecture (DebugVer=6); no PMU, trace, or hw breakpoints.
 * DBG register accesses themselves have no table row and read as Undefined. */
#define OEMU_ID_AA64DFR0_EL1 ((uint64_t)0x00000006)
/* 36-bit PA (covers the 4 GB identity map), 16-bit ASIDs, 4 KB granule only:
 * TGran16/TGran64 read as not-implemented so a guest cannot select them. */
#define OEMU_ID_AA64MMFR0_EL1 ((uint64_t)0x0FF00021)
/* No PAN/UAO/LOR/HAFDBS: zero here is what keeps a Linux guest from emitting
 * MSR PAN/UAO, which the M3 MMU would have to trap (roadmap D6). */
#define OEMU_ID_AA64MMFR1_EL1 ((uint64_t)0x00000000)
/* Cortex-A57 r1p0 -- the QEMU-virt CPU family oemu's board model follows. */
#define OEMU_MIDR_EL1 ((uint64_t)0x411FD080)
/* Bit 31 RES1, affinity fields 0: a single processor; must match the DT the
 * board layer generates in M4, or Linux computes the wrong CPU map. */
#define OEMU_MPIDR_EL1 ((uint64_t)0x80000000)
/* No specific revision identification. */
#define OEMU_REVIDR_EL1 ((uint64_t)0x00000000)
/* Identical to the Cortex-A57 value QEMU's virt machine presents: 64-byte
 * I/D/ERG lines, PIPT L1 (L1Ip=0b11 in the current layout), DIC/IDC clear so
 * a guest performs explicit cache maintenance, which M3 turns into no-ops. */
#define OEMU_CTR_EL0 ((uint64_t)0x8444C004)
/* One cache level, separate I/D (Ctype1=0b011); LoUIS=LoC=LoUU=1, i.e. level
 * 1 is the point of unification and coherency. */
#define OEMU_CLIDR_EL1 ((uint64_t)0x09200003)
/* Write-back, 128 sets, 4-way, 64-byte line: a plain 32 KB L1 as described by
 * CLIDR above. CSSELR is write-ignored, so this is the only cache format. */
#define OEMU_CCSIDR_EL1 ((uint64_t)0x100FE01A)
/* DC ZVA prohibited (DZP=1): oemu implements no data-cache zero operation. */
#define OEMU_DCZID_EL0 ((uint64_t)0x00000010)

/* --- state ---------------------------------------------------------------------- */

typedef struct oemu_sysregs {
  /* The paired register file; SP_EL0 and NZCV are read and written through
   * it. Set once by oemu_sysregs_init and never re-pointed. */
  oemu_regs *regs;

  /* Banked per-exception-level registers, indexed by oemu_el. Slot 0 is
   * architecturally unused (exceptions never target EL0; SP_EL0 lives in
   * regs->sp) and slot 1 indexes EL1. EL2 slots stay zero: no EL2. */
  uint64_t sp_el[OEMU_EL_COUNT]; /* [3] exists for exception entry. There is
                                  * deliberately no SP_EL3 MRS/MSR row: each
                                  * SP_ELx encoding lives in the op1 bank of
                                  * the level above it, and no level sits
                                  * above EL3, so the architecture defines
                                  * no SP_EL3 encoding at all (consistent
                                  * with clang, LLVM, QEMU and Linux, none
                                  * of which list one). */
  uint64_t elr_el[OEMU_EL_COUNT];
  uint64_t spsr_el[OEMU_EL_COUNT];
  uint64_t vbar_el[OEMU_EL_COUNT]; /* [1] and [3] used in M2. */
  uint64_t esr_el[OEMU_EL_COUNT];
  uint64_t far_el[OEMU_EL_COUNT];

  /* EL1 control registers. M2 stores them and resets them; M3 starts
   * honouring the MMU-related bits. */
  uint64_t sctlr_el1;
  uint64_t ttbr0_el1;
  uint64_t ttbr1_el1;
  uint64_t tcr_el1;
  uint64_t mair_el1;
  uint64_t amair_el1;
  uint64_t contextidr_el1;
  uint64_t cpacr_el1;
  uint64_t tpidr_el1;
  uint64_t tpidr_el0;   /* user thread ID, RW at EL0 */
  uint64_t tpidrro_el0; /* user read-only ID; oemu models writes as Undefined
                         * even at EL1 -- refined with M3's per-EL direction */

  /* Constant identification block: written once by init, read-only after. */
  uint64_t midr_el1;
  uint64_t mpidr_el1;
  uint64_t revidr_el1;
  uint64_t ctr_el0;
  uint64_t clidr_el1;
  uint64_t ccsidr_el1;
  uint64_t dczid_el0;
  uint64_t id_aa64pfr0_el1;
  uint64_t id_aa64isar0_el1;
  uint64_t id_aa64isar1_el1;
  uint64_t id_aa64dfr0_el1;
  uint64_t id_aa64mmfr0_el1;
  uint64_t id_aa64mmfr1_el1;

  /* PSTATE, in the architectural bit layout defined by the OEMU_PSTATE_*
   * constants above. */
  uint64_t pstate;
} oemu_sysregs;

/*
 * Prepares a system-register bank for boot at `el`, pairing it with `regs`.
 *
 * Applies every table row's reset value, then composes the boot PSTATE: mode
 * `oemu_pstate_mode(el)`, all four interrupt masks set (the guest unmasks
 * what it wants once it has a vector table), SPSel=1 from EL1 up (the DT
 * booting convention for a kernel entered at EL1) and IL/SS clear. `regs` is
 * not touched: PC and SP_EL0 belong to oemu_cpu_init. Booting at EL2 is a
 * programming error -- oemu implements no EL2 -- and aborts.
 */
void oemu_sysregs_init(oemu_sysregs *sr, oemu_regs *regs, oemu_el el);

/*
 * The table-driven MRS: reads the register named by the 14-bit `sel` encoding
 * into `*out`. Returns OEMU_ERR_UNSUPPORTED for an unimplemented encoding, a
 * read-only register's write counterpart, or an access above the current
 * exception level -- the caller's Undefined-injection signal. Aborts on a
 * NULL state or NULL out: both are caller bugs, not guest behaviour.
 */
OEMU_NODISCARD oemu_status oemu_sysreg_read(const oemu_sysregs *sr, uint32_t sel,
                                            uint64_t *out);

/*
 * The table-driven MSR. Read-only registers and accesses from too low an
 * exception level return OEMU_ERR_UNSUPPORTED (Undefined); write-ignored
 * registers accept and discard the value and return OEMU_OK.
 */
OEMU_NODISCARD oemu_status oemu_sysreg_write(oemu_sysregs *sr, uint32_t sel, uint64_t value);

/*
 * A stable, never-NULL name for any 14-bit encoding: the register's mnemonic
 * for table rows, "unknown" otherwise. For diagnostics and dumps only -- the
 * architecture does not name encodings at runtime.
 */
const char *oemu_sysreg_name(uint32_t sel);

OEMU_END_DECLS

#endif /* OEMU_SYSREG_H */
