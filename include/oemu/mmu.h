/*
 * Stage-1 address translation: the walk, and the bus layer that applies it (M3a).
 *
 * oemu_mmu is a *layer*, not a device: it sits between a vCPU and the
 * physical bus (aspace memops), translates every address the guest touches
 * through the architecture's translation table walk, and forwards the
 * resulting physical address to the bus underneath. A guest with
 * SCTLR_EL1.M = 0 sees the layer as pure identity; a guest with the MMU on
 * sees mappings, permissions and precise faults.
 *
 * Scope (roadmap M3a, deliberately bounded):
 *   - EL1&0 regime only. There is no EL2 (decision D3), so no stage 2, and
 *     the EL3 regime is not modelled: EL3 fetches and data accesses run
 *     untranslated (the firmware that would live at EL3 never executes).
 *   - 4 KiB granule only; ID_AA64MMFR0 honestly advertises 16K/64K as absent.
 *   - 36-bit output address (PARANGE=0b0001); TCR_EL1.PS may only shrink it.
 *   - No FEAT_HAFDBS (ID_AA64MMFR1 = 0): a descriptor with AF=0 takes an
 *     Access Flag fault; the hardware AF update is not modelled.
 *   - No ASID/VMID tagging yet (M3a has no TLB), no contiguous-hint, no
 *     DBM, no GP/BTI, no shareability effects.
 *   - Cacheability is parsed from the descriptor but has no semantic effect
 *     (oemu has no caches): MAIR_EL1 is read and ignored, and the
 *     Device-type alignment rule is deferred to M4, when devices exist.
 *
 * Faults are never returned: a failing access yields OEMU_ERR_FAULT from the
 * memops view, and the *architectural* fault record -- the full ESR_ELx
 * syndrome and FAR value the CPU would report -- is left for the owner to
 * consume with oemu_mmu_take_fault and deliver through oemu_exc_take. The
 * walk itself is a pure function of (sysregs, bus, va, access) and is exposed
 * for white-box testing in src/mmu/mmu_internal.h.
 */
#ifndef OEMU_MMU_H
#define OEMU_MMU_H

#include "oemu/macros.h"
#include "oemu/memops.h"
#include "oemu/status.h"
#include "oemu/sysreg.h"

#include <stdbool.h>
#include <stdint.h>

OEMU_BEGIN_DECLS

/*
 * The fault record a failing access leaves behind: exactly what
 * oemu_exc_take needs for a synchronous delivery. `esr` is the complete
 * syndrome (EC chosen for the access kind and the faulting EL, IL=1, ISS
 * with the DFSC); `far` is the virtual address the access named.
 */
typedef struct oemu_mmu_fault {
  uint32_t esr;
  uint64_t far;
} oemu_mmu_fault;

/*
 * The translation layer. Embedded by value, no allocation, no dispose:
 * `sysregs` points at the paired sysreg block it translates for (the same
 * one the vCPU owns -- control registers are read live, so an MSR to
 * SCTLR_EL1 takes effect on the next access), and `phys` is a copied view of
 * the bus below.
 */
typedef struct oemu_mmu {
  oemu_sysregs *sysregs;
  oemu_memops phys;
  oemu_mmu_fault fault; /* one pending record; valid iff fault_valid */
  bool fault_valid;
} oemu_mmu;

/*
 * Wire one translation layer. Aborts (OEMU_REQUIRE) on NULL arguments or a
 * half-built bus view -- programmer errors, the same contract the vCPU
 * enforces at init. Starting state: no fault pending; whether the MMU is
 * active is decided per access by SCTLR_EL1.M, not here.
 */
void oemu_mmu_init(oemu_mmu *mmu, oemu_sysregs *sysregs, const oemu_memops *phys);

/*
 * The translated bus: a memops view whose fetch32/read/write/validate
 * translate (or apply the SCTLR.A/SA0 alignment checks, or pass through when
 * the MMU is off / running at EL3) and then forward to the physical bus.
 * A failing callback returns OEMU_ERR_FAULT and leaves exactly one fault
 * record pending for oemu_mmu_take_fault.
 */
oemu_memops oemu_mmu_memops(oemu_mmu *mmu);

/*
 * Consume the pending fault, if any (it does not stay pending: a delivered
 * fault must not be delivered twice). True when a record was returned.
 */
bool oemu_mmu_take_fault(oemu_mmu *mmu, oemu_mmu_fault *out);

/*
 * Translate one address without going through the bus layer (white-box
 * entry for the device back door, and for tests). `va` is the virtual
 * address as the guest presented it (top-byte-ignore applied internally per
 * TCR_EL1.TBI0/TBI1); `is_write` selects the store permission,
 * `is_fetch` the execute permission. Returns OEMU_OK with *pa_out set, or
 * OEMU_ERR_FAULT with *fault_out written (if non-NULL) -- without recording
 * anything in `mmu`: this entry is pure with respect to the fault slot.
 */
OEMU_NODISCARD oemu_status oemu_mmu_translate(const oemu_mmu *mmu, uint64_t va, bool is_write,
                                              bool is_fetch, uint64_t *pa_out,
                                              oemu_mmu_fault *fault_out);

OEMU_END_DECLS

#endif /* OEMU_MMU_H */
