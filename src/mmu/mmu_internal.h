/*
 * Internal interface of the mmu module -- NOT part of the public API.
 *
 * The walk splits into the pieces that can be pinned without a bus:
 * geometry (which VA region, which start level, which DFSC for which
 * failure), descriptor decoding, the permission table, and the fault
 * syndrome composition. Each is a pure function here so the tests can sweep
 * every combination directly instead of conjuring page tables into an
 * address space per case.
 *
 * Rules for this pattern (see regs_internal.h): never installed, never
 * included by another module's public header, oemu_<module>_internal_
 * prefix.
 */
#ifndef OEMU_SRC_MMU_INTERNAL_H
#define OEMU_SRC_MMU_INTERNAL_H

#include "oemu/macros.h"
#include "oemu/status.h"

#include <stdbool.h>
#include <stdint.h>

OEMU_BEGIN_DECLS

/* Which failure class the DFSC must encode. The walk knows what broke; only
 * this function knows the bit pattern (Linux arch/arm64/include/asm/esr.h:
 * FSC_FAULT_L(n)=0x04+n, FSC_ACCESS_L(n)=0x08+n, FSC_PERM_L(n)=0x0C+n,
 * FSC_ADDRSZ_L(n)=0x00+n with the negative levels at 0x29/0x2C, plus
 * alignment 0x21 and the not-on-a-walk translation 0x2C). */
typedef enum oemu_mmu_fault_class {
  OEMU_MMU_FAULT_TRANSLATION = 0,     /* level 0..3 */
  OEMU_MMU_FAULT_TRANSLATION_NO_WALK, /* not on a table walk: level -1, 0x2C */
  OEMU_MMU_FAULT_ACCESS_FLAG,         /* level 1..3 (descriptor AF=0) */
  OEMU_MMU_FAULT_PERMISSION,          /* level 1..3 */
  OEMU_MMU_FAULT_ADDRESS_SIZE,        /* level 0..3, or -1 for the TTBR base */
  OEMU_MMU_FAULT_ALIGNMENT            /* level -1: 0x21 */
} oemu_mmu_fault_class;

/* The Data-Abort-class DFSC for one class at one level. Levels outside the
 * class's range are programmer errors (OEMU_REQUIRE). */
uint8_t oemu_mmu_internal_dfsc(oemu_mmu_fault_class cls, int level);

/*
 * The starting level for a 4 KiB-granule walk of an `inputsize`-bit address
 * space (inputsize = 64 - TnSZ, i.e. the architectural VA width): the number
 * of table strides needed to consume the input, subtracted from the bottom
 * (level 3 = 4 KiB page). Returns 0, 1 or 2 for a usable width and -1 when
 * the width is outside the 4 KiB granule's supported range (TnSZ 16..39),
 * which the caller reports as a level-0 translation fault.
 */
int oemu_mmu_internal_start_level(unsigned inputsize);

/*
 * The region a translated address belongs to. With top-byte-ignore the tag
 * rides in bits [63:56] and is stripped before the split; bit [55] then
 * chooses TTBR0 (false, low region) or TTBR1 (true, high region).
 */
bool oemu_mmu_internal_use_ttbr1(uint64_t va);

/*
 * The address-space sign rule: bits [addrsize-1:inputsize] of the (tag
 * stripped) address must be the sign extension of the region -- all zeroes
 * for TTBR0, all ones for TTBR1 -- or the address falls in the unmapped
 * gap and takes a translation fault at the starting level. `addrsize` is
 * 64 or 56 (top-byte-ignore strips one byte).
 */
bool oemu_mmu_internal_region_ok(uint64_t va, unsigned inputsize, unsigned addrsize,
                                 bool ttbr1);

/*
 * The SCTLR_EL1 alignment policy for one access: true when the access must
 * be naturally aligned (and a violation reports an Alignment fault rather
 * than executing). EL1 accesses obey SCTLR.A; EL0 accesses obey SCTLR.SA0.
 */
bool oemu_mmu_internal_alignment_required(const oemu_sysregs *sr, bool el0);

/*
 * The permission verdict for one walk result, after the table-descriptor
 * attributes have been merged: the two-bit AP field, XN (UXN) and PXN, the
 * access kind, and whether the access is at EL0. The table (ARM ARM D8,
 * cross-checked against QEMU ptw.c get_S1prot):
 *
 *   AP[2:1]  EL1 read  EL1 write  EL0 read  EL0 write
 *   0b00     yes       yes        no        no
 *   0b01     yes       yes        yes       yes
 *   0b10     yes       no         no        no
 *   0b11     yes       no         yes       no
 *
 * (0b10 is kernel read-only -- EL0 sees nothing; 0b11 is a user read-only
 * mapping, which is what Linux emits for r-- pages: AP[1] user bit plus
 * AP[2] read-only bit.)
 *
 * Execution needs read permission plus the per-EL execute bits: XN blocks
 * EL0, PXN blocks EL1.
 */
bool oemu_mmu_internal_permits(unsigned ap, bool xn, bool pxn, bool el0, bool is_write,
                               bool is_fetch);

/*
 * Compose the full ESR_ELx syndrome for one mmu fault: EC from the access
 * kind and whether the fault is delivered to a higher EL (routing decides
 * that from the current EL), IL=1 (every AArch64 raised exception), ISS =
 * DFSC plus the Data-Abort WnR bit. ISV stays 0: a translation failure
 * cannot prove which element of an access it blocked, and the alignment
 * policy is checked by the layer that knows the width.
 */
uint32_t oemu_mmu_internal_esr(bool to_lower_el, bool is_fetch, bool is_write, uint8_t dfsc);

/* One descriptor's decoded walk-relevant properties. */
typedef struct oemu_mmu_desc {
  bool valid;       /* bit [0] */
  bool table;       /* bits [1:0] == 0b11 (only meaningful at levels 0..2) */
  bool block_page;  /* bits [1:0] == 0b01 */
  uint64_t address; /* bits [47:12], address-size-checked by the caller */
  bool af;          /* bit [10] */
  unsigned ap;      /* bits [7:6]: AP[2] in bit 1, AP[1] in bit 0 */
  bool xn;          /* bit [54] (UXN) */
  bool pxn;         /* bit [53] */
  bool table_pxn;   /* bit [59], table descriptors only */
  bool table_xn;    /* bit [60] */
  bool table_apt0;  /* bit [61]: force AP[1] low (block EL0 below) */
  bool table_apt1;  /* bit [62]: force AP[2] high (block EL1 writes below) */
} oemu_mmu_desc;

oemu_mmu_desc oemu_mmu_internal_decode(uint64_t descriptor);

/*
 * The table-descriptor attributes that must propagate downward (ARM ARM:
 * table bits [62:59] constrain everything below them; NSTable is ignored --
 * oemu is single-world). Applied by ORing into the accumulated table attrs.
 */
uint32_t oemu_mmu_internal_table_attrs(uint64_t descriptor);

/* The 64-bit VA-indexed offset into a 4 KiB, 8-byte-entry table at `level`
 * (0..3), already shifted to bit 3: the caller ORs it onto the table base. */
uint64_t oemu_mmu_internal_index(uint64_t va, unsigned level);

OEMU_END_DECLS

#endif /* OEMU_SRC_MMU_INTERNAL_H */
