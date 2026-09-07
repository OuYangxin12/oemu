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
 *   - Direct-mapped TLB since M3b (see the struct): every TLBI executes as
 *     flush-all, so no ASID- or VMID-precise invalidation, and the tag
 *     fields that only such invalidation would compare are stored, pinned,
 *     and inert until the feature that reads them lands. No
 *     contiguous-hint, no DBM, no GP/BTI, no shareability effects.
 *   - Cacheability is parsed from the descriptor but has no semantic effect
 *     (oemu has no caches): MAIR_EL1 is read and ignored, and the
 *     Device-type alignment rule is deferred to M4, when devices exist.
 *
 * Faults are never returned: a failing access yields OEMU_ERR_FAULT from the
 * memops view, and the *architectural* fault record -- the full ESR_ELx
 * syndrome and FAR value the CPU would report -- is left for the owner to
 * consume with oemu_mmu_take_fault and deliver through oemu_exc_take. The
 * walk itself is a pure function of (sysregs, bus, va, access); the TLB in
 * front of it only ever serves what that function returned, and the
 * bypassed walk is exposed for white-box testing (the parity oracle) in
 * src/mmu/mmu_internal.h.
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
 * One direct-mapped TLB entry (M3b). The set index is not stored -- it is
 * VA[23:12] of the address under translation, so the tag can carry the
 * whole address and a hit is an exact match, never a masked prefix.
 *
 * `level` doubles as the valid bit and the leaf size (0 = free; the 4 KiB
 * granule never terminates a walk at level 0, so a valid entry's level is
 * 1..3). `asid` is TTBR[63:48] at fill time and `vmid` is the stage-2 tag:
 * both are stored, pinned, and inert -- oemu invalidates wholesale, so
 * nothing compares them yet; the day precise invalidation lands, the tag
 * is already what it must be. `flags` packs the verdict inputs, not the
 * verdict: a hit re-runs the permission table with them (see
 * oemu_mmu_internal_permits), so an AP/XN disagreement between cache and
 * walk is structurally impossible.
 */
#define OEMU_MMU_TLB_ENTRIES 4096U

typedef struct oemu_tlb_entry {
  uint64_t va;        /* the tagged address after top-byte-ignore, page-aligned */
  uint64_t pa;        /* the leaf's output page base, address-size-checked */
  uint32_t asid;      /* TTBR[63:48] at fill time; see the note: pinned, inert */
  uint8_t level;      /* walk level of the leaf; 0 = free */
  uint8_t flags;      /* bit 0 nG; bits 2:1 AP; bit 3 XN; bit 4 PXN */
  uint8_t vmid;       /* always 0: no stage 2 (decision D3); pinned, inert */
  uint8_t reserved0_; /* explicit pad: the layout is tested, not incidental */
} oemu_tlb_entry;

/*
 * The translation layer. Embedded by value, no allocation, no dispose:
 * `sysregs` points at the paired sysreg block it translates for (the same
 * one the vCPU owns -- control registers are read live, so an MSR to
 * SCTLR_EL1 takes effect on the next access), and `phys` is a copied view of
 * the bus below. The TLB rides along by value too, so the struct is ~96 KiB
 * (4096 entries at 24 B) and a stack object holding one needs that much
 * stack -- fine on the 8 MiB main stack, and the fixtures and vCPU owners
 * are heap- or BSS-side anyway.
 */
typedef struct oemu_mmu {
  oemu_sysregs *sysregs;
  oemu_memops phys;
  oemu_mmu_fault fault; /* one pending record; valid iff fault_valid */
  bool fault_valid;
  oemu_tlb_entry tlb[OEMU_MMU_TLB_ENTRIES];
  /* The control epoch: the five registers a change to must invalidate
   * every entry (SCTLR, TCR, TTBR0, TTBR1, MAIR -- the architecture's
   * break-before-make contract). Compared live on every lookup; a stale
   * epoch flushes before the lookup proceeds, which is also what makes a
   * same-value re-write harmless and any-value change sufficient. */
  uint64_t epoch[5];
  bool epoch_valid; /* false until the first translation has read them */
  uint64_t tlb_hits;
  uint64_t tlb_misses;
  uint64_t tlb_flushes;
} oemu_mmu;

/*
 * Wire one translation layer. Aborts (OEMU_REQUIRE) on NULL arguments or a
 * half-built bus view -- programmer errors, the same contract the vCPU
 * enforces at init. Starting state: no fault pending, empty TLB, epoch
 * unprimed (the first lookup takes it); whether the MMU is active is
 * decided per access by SCTLR_EL1.M, not here.
 */
void oemu_mmu_init(oemu_mmu *mmu, oemu_sysregs *sysregs, const oemu_memops *phys);

/*
 * Invalidate every entry and count one flush. What every TLBI instruction
 * executes into (oemu's answer to any invalidation request is the whole
 * cache -- precise invalidation is a deliberate later step), and what the
 * epoch check applies itself when a control register moved. The fault slot
 * is untouched: an invalidation is not a fault and leaves no state.
 */
void oemu_mmu_flush_all(oemu_mmu *mmu);

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
 *
 * Not const in the layer itself: the call may serve from or refill the
 * TLB, which is cache state, not architectural state. The bypass-the-TLB
 * twin for parity testing lives in src/mmu/mmu_internal.h.
 */
OEMU_NODISCARD oemu_status oemu_mmu_translate(oemu_mmu *mmu, uint64_t va, bool is_write,
                                              bool is_fetch, uint64_t *pa_out,
                                              oemu_mmu_fault *fault_out);

OEMU_END_DECLS

#endif /* OEMU_MMU_H */
