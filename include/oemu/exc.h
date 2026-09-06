/*
 * AArch64 exception delivery (M2b).
 *
 * Turns an oemu_regs + oemu_sysregs pair into a CPU that can take exceptions:
 * `oemu_exc_take` performs the architectural entry sequence (SP bank switch,
 * SPSR/ELR/ESR/FAR updates, masked h-mode PSTATE, PC to the vector), and
 * `oemu_exc_eret` performs the return with its permission checks. The fault
 * *sources* -- SVC/BRK/undefined/abort -- have convenience helpers that build
 * the syndrome and route the target; the generic take() covers the kinds
 * without an instruction syndrome (IRQ, FIQ) that the vCPU (M2c) will raise.
 *
 * Routing is simplified and documented (roadmap D3/D5): synchronous and
 * interrupt exceptions go to EL1, but never downward from EL3 (an EL3 guest
 * takes its own exceptions); SMC and HVC go to EL3, where PSCI (M4) will
 * handle them, until then the executor raises Undefined for them. The AArch32
 * vector groups exist (offset 0x600) but are never selected: oemu guests are
 * AArch64.
 *
 * The exception-state invariant the entry/eret paths maintain:
 * `oemu_regs.sp` mirrors the stack-pointer bank PSTATE selects -- SP_EL0 of
 * `sp_el[0]` while in EL0/SP_EL0 mode, `sp_el[el]` while at EL>0 with SPSel=1
 * -- and the inactive banks hold their values, so a bank switch saves one
 * into the table and loads the next. `oemu_sysregs_init` seeds the boot
 * bank so the first switch is well-defined.
 *
 * Delivery never fails (entry itself cannot fault; vector fetch faults go
 * through the abort path on the *next* step), so these functions return
 * void: NULL arguments are caller bugs and abort via OEMU_REQUIRE.
 */
#ifndef OEMU_EXC_H
#define OEMU_EXC_H

#include "oemu/macros.h"
#include "oemu/regs.h"
#include "oemu/sysreg.h"

#include <stdbool.h>
#include <stdint.h>

OEMU_BEGIN_DECLS

/* --- syndrome: ESR_ELx EC field --------------------------------------------- */
/*
 * Exception classes from the ARM ARM, cross-checked against Linux
 * arch/arm64/include/asm/esr.h and QEMU target/arm/syndrome.h (both agree).
 * Only the classes oemu can raise are listed; the numbering is the
 * architectural one.
 */
typedef enum oemu_exc_ec {
  OEMU_EXC_EC_UNKNOWN = 0x00,      /* undefined instruction */
  OEMU_EXC_EC_WFX = 0x01,          /* WFI/WFE trapped (SCR/TWEDR/HCR/FGT) */
  OEMU_EXC_EC_FP_ASIMD = 0x07,     /* trapped FP/SIMD access (CPACR.TFPE=0) */
  OEMU_EXC_EC_SVC64 = 0x15,        /* SVC, AArch64 */
  OEMU_EXC_EC_HVC64 = 0x16,        /* HVC, AArch64 */
  OEMU_EXC_EC_SMC64 = 0x17,        /* SMC, AArch64 */
  OEMU_EXC_EC_SYSREG = 0x18,       /* trapped MSR/MRS/SYS/TLBI/DC access */
  OEMU_EXC_EC_ILLEGAL_ERET = 0x1A, /* ERET with SPSR.IL set */
  OEMU_EXC_EC_IABORT_LOWER = 0x20,
  OEMU_EXC_EC_IABORT_SAME = 0x21,
  OEMU_EXC_EC_DABORT_LOWER = 0x24,
  OEMU_EXC_EC_DABORT_SAME = 0x25,
  OEMU_EXC_EC_SERROR = 0x2F,     /* kind SERROR delivers the caller's syndrome; the
                                  * value is named so test/diagnostic code avoids
                                  * magic numbers */
  OEMU_EXC_EC_BREAKPOINT = 0x30, /* hardware breakpoint (not raised; named for
                                  * completeness -- BRK is 0x3C) */
  OEMU_EXC_EC_BRK64 = 0x3C       /* BRK, AArch64 */
} oemu_exc_ec;

/* Exception kinds: which vector within the table. Reset has no vector (the
 * boot path inits state instead); so SError is reserved until a GIC exists. */
typedef enum oemu_exc_kind {
  OEMU_EXC_KIND_SYNC = 0, /* SVC, UDF, aborts, BRK: ESR + maybe FAR valid */
  OEMU_EXC_KIND_IRQ = 1,  /* leaves ESR untouched (architecturally RES0) */
  OEMU_EXC_KIND_FIQ = 2,
  OEMU_EXC_KIND_SERROR = 3 /* ESR written, FAR untouched */
} oemu_exc_kind;

/*
 * Delivery target for an exception raised at `from`: EL3 keeps its own
 * exceptions (nothing above it in this model); everything else goes to EL1,
 * the kernel's level, because no EL2 exists to route through and the firmware
 * that would own EL3 is not running (roadmap D3).
 */
oemu_el oemu_exc_route(oemu_el from);

/*
 * The AArch64 vector-table offset for one exception: VBAR + 0x000 same-EL
 * using SP_EL0, +0x200 same-EL using SP_ELx, +0x400 lower EL using AArch64
 * (AArch32's 0x600 group exists but oemu guests never select it), then
 * +0x000 Sync, +0x080 IRQ, +0x100 FIQ, +0x180 SError. `sp_sel` describes the
 * *interrupted* state's SPSel, which selects between the two same-EL groups
 * (the rule QEMU implements).
 */
uint64_t oemu_exc_vector_offset(bool same_el, bool sp_sel, oemu_exc_kind kind);

/* The routing rule for SMC/HVC: they are monitor calls by definition. */
#define OEMU_EXC_TARGET_MONITOR OEMU_EL3

/*
 * Take one exception. `target` is the delivery level (use oemu_exc_route);
 * `esr` is the syndrome written for SYNC/SERROR kinds; `far` is written only
 * when `far_valid` (the abort kinds). Entry order: SP saved into the bank
 * PSTATE selects, VBAR+vector computed, SPSR/ELR/ESR/FAR written, then
 * PSTATE = target h-mode with all interrupts masked (DAIF=0b1111) and IL=1
 * (an exception return from a nested, corrupted context is then Illegal
 * ERET), and finally PC = vector. The faulting instruction's address is
 * `regs.pc` unchanged -- delivery must be called before any PC advance so
 * ELR captures it (the precise-exception contract).
 */
void oemu_exc_take(oemu_regs *regs, oemu_sysregs *sysregs, oemu_exc_kind kind, oemu_el target,
                   uint32_t esr, uint64_t far, bool far_valid);

/*
 * Return from an exception at the current level (ERET): validate SPSR first
 * (IL set -> deliver Illegal ERET; mode field illegal or targeting a higher
 * level -> deliver Undefined, both to the level ERET was executing at), then
 * restore PSTATE/SP/PC from SPSR_ELx/ELR_ELx. AArch32 returns are Undefined
 * for this model: there is no AArch32 state to return to.
 */
void oemu_exc_eret(oemu_regs *regs, oemu_sysregs *sysregs);

/* --- fault sources: build the syndrome, route, take -------------------------- */

/* Undefined instruction: ISS carries bits [24:0] of the word (the IL bit of
 * ESR is set for AArch64-generated exceptions). */
void oemu_exc_undefined(oemu_regs *regs, oemu_sysregs *sysregs, uint32_t insn);

void oemu_exc_svc(oemu_regs *regs, oemu_sysregs *sysregs, uint16_t imm16);
void oemu_exc_brk(oemu_regs *regs, oemu_sysregs *sysregs, uint16_t imm16);

/*
 * SMC/HVC: architecturally monitor calls; oemu raises Undefined until PSCI
 * (M4) can service them. The helpers exist so the executor's switch is
 * honest about what these instructions are.
 */
void oemu_exc_smc(oemu_regs *regs, oemu_sysregs *sysregs, uint16_t imm16);
void oemu_exc_hvc(oemu_regs *regs, oemu_sysregs *sysregs, uint16_t imm16);

/*
 * Instruction abort (fetch fault): FAR = the faulting fetch address, DFSC
 * = 0b000100 translation fault level 1 -- oemu's address spaces report
 * unmapped ranges, which reads exactly like a missing first-level mapping.
 */
void oemu_exc_instruction_abort(oemu_regs *regs, oemu_sysregs *sysregs, uint64_t far);

/*
 * Data abort (memory fault): `log2_size` is the access width (0..3), giving
 * ISS.SAS, `is_write` the WnR bit, and `isv` advertises both to a guest
 * handler; pair accesses pass isv=false because the architecture leaves
 * SRT/SAS undefined for them. `dfsc` selects the fault class (0x01
 * alignment, 0x04/0x0C/0x2C translation faults...).
 */
void oemu_exc_data_abort(oemu_regs *regs, oemu_sysregs *sysregs, uint64_t far,
                         unsigned log2_size, bool is_write, bool isv, uint8_t dfsc);

/* A stable, never-NULL name for an EC value, for diagnostics and dumps. */
const char *oemu_exc_ec_name(oemu_exc_ec ec);

OEMU_END_DECLS

#endif /* OEMU_EXC_H */
