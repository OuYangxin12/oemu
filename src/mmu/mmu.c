/*
 * Stage-1 address translation. See include/oemu/mmu.h for the contract and
 * src/mmu/mmu_internal.h for the pure decision functions.
 *
 * The walk mirrors the ARM ARM's AArch64 translation pseudocode restricted
 * to what the ID registers advertise oemu can do: the EL1&0 regime, 4 KiB
 * granule, no FEAT_HAFDBS/LPA/LPA2/ASID tagging. Every divergence from the
 * pseudocode is a consequence of a value the guest can read back, never a
 * silent shortcut.
 *
 * The bus layer above the walk adds what the architecture puts around
 * translation, in the order hardware reports it: a mapping fault (the walk
 * itself), then the alignment policy (meaningful only for a translated
 * address), then the physical bus's own verdict -- no region backs the
 * physical address, or the bus cannot serve the transfer -- reported with
 * the translation-fault-without-a-level code (DFSC 0x2C, Linux's
 * FSC_FAULT_nL). Misaligned accesses the policy permits are split into byte
 * transfers, because the walk may map the two halves of a word to physical
 * pages the flat bus cannot join.
 */
#include "oemu/mmu.h"

#include "oemu/check.h"
#include "oemu/exc.h"
#include "oemu/memory.h"

#include <stddef.h>

#include "mmu_internal.h"

/* --- control register bit positions ------------------------------------------ */
/* TCR_EL1. Field positions per the ARM ARM, cross-checked against Linux
 * arch/arm64/tools/sysreg. */
#define TCR_T0SZ(t) ((unsigned)((t) & 0x3FU))
#define TCR_T1SZ(t) ((unsigned)(((t) >> 16) & 0x3FU))
#define TCR_PS(t)   ((unsigned)(((t) >> 27) & 0x3FU))
#define TCR_EPD0    ((uint64_t)1U << 7U)
#define TCR_EPD1    ((uint64_t)1U << 23U)
#define TCR_TBI0    ((uint64_t)1U << 24U)
#define TCR_TBI1    ((uint64_t)1U << 63U)

/* SCTLR_EL1. The AArch64 alignment policy is two bits, one per EL: A gates
 * EL1 (the SP-alignment class included -- oemu reports both classes as one
 * architectural Alignment fault), SA0 gates EL0. */
#define SCTLR_M   ((uint64_t)1U << 0U)
#define SCTLR_A   ((uint64_t)1U << 1U)
#define SCTLR_SA0 ((uint64_t)1U << 4U)

/* --- descriptor bit positions ------------------------------------------------ */
#define DESC_VALID     ((uint64_t)1U << 0U)
#define DESC_AP2       ((uint64_t)1U << 7U)
#define DESC_AP1       ((uint64_t)1U << 6U)
#define DESC_AF        ((uint64_t)1U << 10U)
#define DESC_PXN       ((uint64_t)1U << 53U)
#define DESC_XN        ((uint64_t)1U << 54U)
#define DESC_ADDR_MASK (UINT64_C(0x0000FFFFFFFFF000)) /* bits [47:12] */

/* Table descriptors: constraint bits that bind everything below them. */
#define TDESC_PXN  ((uint64_t)1U << 59U)
#define TDESC_XN   ((uint64_t)1U << 60U)
#define TDESC_APT0 ((uint64_t)1U << 61U)
#define TDESC_APT1 ((uint64_t)1U << 62U)
/* NSTable (bit 63) has no effect: oemu implements only the Non-secure world
 * (roadmap D3). */

/* Output address size from the PS encoding (ARM's shared mapping), clamped
 * by what ID_AA64MMFR0.PARANGE advertises (0b0001 -> 36 bits). */
static const uint8_t g_pamax_map[8] = {32U, 36U, 40U, 42U, 44U, 48U, 52U, 52U};
#define MMU_PARANGE_ID 1U

#define MMU_PAGE_SHIFT 12U
#define MMU_PAGE_SIZE  (UINT64_C(1) << MMU_PAGE_SHIFT)

/* The bit position the index field at a level starts at -- and, because a
 * leaf covers everything below its index field, the leaf's size in bits:
 * 39/30/21/12 for levels 0..3 of the 4 KiB granule. */
static unsigned level_shift(unsigned level) {
  return 12U + (3U - level) * 9U;
}

/* The table attribute bits (ARM table descriptor [62:59]) as a mask set:
 * PXN, XN, APTable[0], APTable[1], NSTable. */
#define TATTR_PXN  0x01U
#define TATTR_XN   0x02U
#define TATTR_APT0 0x04U
#define TATTR_APT1 0x08U

/* --- pure internals ---------------------------------------------------------- */

uint8_t oemu_mmu_internal_dfsc(oemu_mmu_fault_class cls, int level) {
  switch (cls) {
    case OEMU_MMU_FAULT_TRANSLATION:
      OEMU_REQUIRE((level >= 0) && (level <= 3), "translation fault level");
      return (uint8_t)(0x04U + (unsigned)level);
    case OEMU_MMU_FAULT_TRANSLATION_NO_WALK:
      return 0x2CU; /* Linux's FSC_FAULT_nL: refused, no level applies */
    case OEMU_MMU_FAULT_ACCESS_FLAG:
      OEMU_REQUIRE((level >= 1) && (level <= 3), "access flag fault level");
      return (uint8_t)(0x08U + (unsigned)level);
    case OEMU_MMU_FAULT_PERMISSION:
      OEMU_REQUIRE((level >= 1) && (level <= 3), "permission fault level");
      return (uint8_t)(0x0CU + (unsigned)level);
    case OEMU_MMU_FAULT_ADDRESS_SIZE:
      /* The negative levels are the awkward ones: -1 (the TTBR base out of
       * range, no walk yet) is 0x29; any real level is 0x00 + level. Linux
       * spells this out in arch/arm64/include/asm/esr.h. */
      if (level < 0) {
        return (level == -1) ? 0x29U : 0x2CU;
      }
      return (uint8_t)(0x00U + (unsigned)level);
    case OEMU_MMU_FAULT_ALIGNMENT:
      return 0x21U;
  }
  OEMU_REQUIRE(false, "unknown fault class");
  return 0U; /* unreachable: the REQUIRE above aborts */
}

int oemu_mmu_internal_start_level(unsigned inputsize) {
  /* TnSZ 16..39 (widths 25..48) is the 4 KiB granule's supported range.
   * Anything else is the guest programming a geometry this granule cannot
   * walk; QEMU's tsz_oob path answers with a level-0 translation fault. */
  if ((inputsize < 25U) || (inputsize > 48U)) {
    return -1;
  }
  return (int)(4U - ((inputsize - 4U) / 9U));
}

bool oemu_mmu_internal_use_ttbr1(uint64_t va) {
  return ((va >> 55U) & 1U) != 0U;
}

bool oemu_mmu_internal_region_ok(uint64_t va, unsigned inputsize, unsigned addrsize,
                                 bool ttbr1) {
  if (inputsize >= addrsize) {
    return true; /* the region fills the address space: no gap to miss */
  }
  const unsigned width = addrsize - inputsize;
  const uint64_t mask = (width >= 64U) ? UINT64_MAX : ((UINT64_C(1) << width) - 1U);
  const uint64_t top = (va >> inputsize) & mask;
  return ttbr1 ? (top == mask) : (top == 0U);
}

bool oemu_mmu_internal_alignment_required(const oemu_sysregs *sr, bool el0) {
  const uint64_t bit = el0 ? SCTLR_SA0 : SCTLR_A;
  return (sr->sctlr_el1 & bit) != 0U;
}

oemu_mmu_desc oemu_mmu_internal_decode(uint64_t descriptor) {
  oemu_mmu_desc d;
  d.valid = (descriptor & DESC_VALID) != 0U;
  d.table = ((descriptor & 3U) == 3U);
  d.block_page = ((descriptor & 3U) == 1U);
  d.address = descriptor & DESC_ADDR_MASK;
  d.af = (descriptor & DESC_AF) != 0U;
  d.ap = (unsigned)((descriptor & DESC_AP2) != 0U) * 2U +
         (unsigned)((descriptor & DESC_AP1) != 0U);
  d.xn = (descriptor & DESC_XN) != 0U;
  d.pxn = (descriptor & DESC_PXN) != 0U;
  d.table_pxn = (descriptor & TDESC_PXN) != 0U;
  d.table_xn = (descriptor & TDESC_XN) != 0U;
  d.table_apt0 = (descriptor & TDESC_APT0) != 0U;
  d.table_apt1 = (descriptor & TDESC_APT1) != 0U;
  return d;
}

uint32_t oemu_mmu_internal_table_attrs(uint64_t descriptor) {
  /* The five constraint bits, kept in their own space so the caller can OR
   * them across levels and apply them once, on the leaf. */
  return (uint32_t)((descriptor >> 59U) & 0x1FU);
}

bool oemu_mmu_internal_permits(unsigned ap, bool xn, bool pxn, bool el0, bool is_write,
                               bool is_fetch) {
  /* The table from the header comment: EL0 needs AP[1] for anything, and
   * AP[2] (bit 1 of `ap`) denies writes wherever it lands. */
  const bool user = (ap & 1U) != 0U;
  const bool ro = (ap & 2U) != 0U;
  bool read_ok;
  bool write_ok;
  if (el0) {
    read_ok = user;         /* 0b01 and 0b11 are the user-visible entries */
    write_ok = user && !ro; /* only 0b01 is user-writable */
  } else {
    read_ok = true;
    write_ok = !ro;
  }
  if (is_fetch) {
    return el0 ? (read_ok && !xn) : !pxn; /* XN is EL0's gate, PXN EL1's */
  }
  return is_write ? write_ok : read_ok;
}

uint32_t oemu_mmu_internal_esr(bool to_lower_el, bool is_fetch, bool is_write, uint8_t dfsc) {
  oemu_exc_ec ec;
  if (is_fetch) {
    ec = to_lower_el ? OEMU_EXC_EC_IABORT_LOWER : OEMU_EXC_EC_IABORT_SAME;
  } else {
    ec = to_lower_el ? OEMU_EXC_EC_DABORT_LOWER : OEMU_EXC_EC_DABORT_SAME;
  }
  /* IL=1 for every AArch64-raised exception. The ISS carries the DFSC and,
   * for data aborts only, WnR: the instruction abort ISS has no WnR bit to
   * set. ISV/SET stay 0: a translation failure cannot prove which element of
   * a multi-element access it blocked. */
  return ((uint32_t)ec << 26U) | (1U << 25U) | dfsc |
         ((!is_fetch && is_write) ? (1U << 6U) : 0U);
}

uint64_t oemu_mmu_internal_index(uint64_t va, unsigned level) {
  return ((va >> level_shift(level)) & 0x1FFU) << 3U;
}

/* --- the walk ---------------------------------------------------------------- */

/* What the walk knows when it fails: the fault class and the level it was
   standing on. The access kind (fetch/read/write) is only complete at the
   bus wrapper, which composes the final ESR. */
typedef struct walk_fault {
  oemu_mmu_fault_class cls;
  int level;
  uint64_t far;
} walk_fault;

static oemu_status walk(const oemu_sysregs *sr, const oemu_memops *phys, uint64_t va,
                        oemu_el cur, bool is_write, bool is_fetch, uint64_t *pa_out,
                        walk_fault *wf) {
  const uint64_t sctlr = sr->sctlr_el1;

  /* EL3 runs untranslated: SCTLR_EL3 is not modelled because the modelled
   * firmware never executes (roadmap D4). */
  if (cur >= OEMU_EL3) {
    *pa_out = va;
    return OEMU_OK;
  }
  if ((sctlr & SCTLR_M) == 0U) {
    *pa_out = va; /* Stage 1 disabled: identity for both EL1 and EL0 */
    return OEMU_OK;
  }

  const uint64_t tcr = sr->tcr_el1;
  const bool ttbr1 = oemu_mmu_internal_use_ttbr1(va);
  const bool tbi = (tcr & (ttbr1 ? TCR_TBI1 : TCR_TBI0)) != 0U;
  if (tbi) {
    va &= ~(UINT64_C(0xFF) << 56); /* the tag is not part of the address, nor of the FAR */
  }

  const unsigned tsz = ttbr1 ? TCR_T1SZ(tcr) : TCR_T0SZ(tcr);
  const unsigned inputsize = 64U - tsz;
  const unsigned addrsize = tbi ? 56U : 64U;
  const unsigned ps = (TCR_PS(tcr) > MMU_PARANGE_ID) ? MMU_PARANGE_ID : TCR_PS(tcr);
  const unsigned out_bits = g_pamax_map[ps];

  int level = oemu_mmu_internal_start_level(inputsize);

#define MMU_FAULT(c, lvl)  \
  do {                     \
    wf->cls = (c);         \
    wf->level = (lvl);     \
    wf->far = va;          \
    return OEMU_ERR_FAULT; \
  } while (0)

  if (level < 0) {
    MMU_FAULT(OEMU_MMU_FAULT_TRANSLATION, 0); /* a width the granule cannot walk */
  }
  if (!oemu_mmu_internal_region_ok(va, inputsize, addrsize, ttbr1)) {
    MMU_FAULT(OEMU_MMU_FAULT_TRANSLATION, level); /* in the gap between the regions */
  }
  if ((tcr & (ttbr1 ? TCR_EPD1 : TCR_EPD0)) != 0U) {
    MMU_FAULT(OEMU_MMU_FAULT_TRANSLATION, level); /* walk disabled for this region */
  }

  /* TTBR_EL1: base in bits [47:1] (bit 0 is CnP, [63:48] the ASID), aligned
   * to the table the start level indexes. */
  uint64_t base = (ttbr1 ? sr->ttbr1_el1 : sr->ttbr0_el1) & ((UINT64_C(1) << 48U) - 2U);
  if ((base >> out_bits) != 0U) {
    MMU_FAULT(OEMU_MMU_FAULT_ADDRESS_SIZE, -1); /* bits above the output range */
  }
  base &= ~(MMU_PAGE_SIZE - 1U);

  uint32_t tattrs = 0U;
  for (;;) {
    uint64_t raw = 0U;
    if (phys->read(phys->ctx, base | oemu_mmu_internal_index(va, (unsigned)level),
                   OEMU_MEM_DWORD, false, &raw) != OEMU_OK) {
      MMU_FAULT(OEMU_MMU_FAULT_TRANSLATION_NO_WALK, -1); /* the table page is not backed */
    }
    const oemu_mmu_desc d = oemu_mmu_internal_decode(raw);

    if (!d.valid) {
      MMU_FAULT(OEMU_MMU_FAULT_TRANSLATION, level);
    }
    if (d.table) {
      if (level >= 3) {
        MMU_FAULT(OEMU_MMU_FAULT_TRANSLATION, 3); /* no level below the page level */
      }
      tattrs |= oemu_mmu_internal_table_attrs(raw);
      base = d.address;
      if ((base >> out_bits) != 0U) {
        MMU_FAULT(OEMU_MMU_FAULT_ADDRESS_SIZE, level);
      }
      base &= ~(MMU_PAGE_SIZE - 1U);
      level++;
      continue;
    }
    if (!d.block_page) {
      MMU_FAULT(OEMU_MMU_FAULT_TRANSLATION, level); /* bits [1:0] == 0b10: reserved */
    }
    if (level == 0) {
      /* A block at level 0 is FEAT_LPA2 territory, which the ID registers
       * exclude, so it carries the reserved encoding's answer. */
      MMU_FAULT(OEMU_MMU_FAULT_TRANSLATION, 0);
    }

    const uint64_t page_size = UINT64_C(1) << level_shift((unsigned)level);
    const uint64_t addr = (d.address & ~(page_size - 1U)) | (va & (page_size - 1U));
    if ((addr >> out_bits) != 0U) {
      MMU_FAULT(OEMU_MMU_FAULT_ADDRESS_SIZE, level);
    }

    unsigned ap = d.ap;
    bool xn = d.xn;
    bool pxn = d.pxn;
    if ((tattrs & TATTR_PXN) != 0U) {
      pxn = true;
    }
    if ((tattrs & TATTR_XN) != 0U) {
      xn = true;
    }
    if ((tattrs & TATTR_APT0) != 0U) {
      ap &= ~1U; /* force AP[1] low: no EL0 access anywhere below */
    }
    if ((tattrs & TATTR_APT1) != 0U) {
      ap |= 2U; /* force AP[2] high: no EL1 writes anywhere below */
    }

    if (!d.af) {
      /* No FEAT_HAFDBS: software owns the access flag, and the architecture's
       * answer to touching a clear one is an Access Flag fault. */
      MMU_FAULT(OEMU_MMU_FAULT_ACCESS_FLAG, level);
    }
    const bool el0 = cur == OEMU_EL0;
    if (!oemu_mmu_internal_permits(ap, xn, pxn, el0, is_write, is_fetch)) {
      MMU_FAULT(OEMU_MMU_FAULT_PERMISSION, level);
    }

    *pa_out = addr;
    return OEMU_OK;
  }
#undef MMU_FAULT
}

/* --- fault plumbing ---------------------------------------------------------- */

static bool to_lower_from(oemu_el cur) {
  return cur < oemu_exc_route(cur);
}

static uint32_t compose_esr(oemu_el cur, bool is_fetch, bool is_write, oemu_mmu_fault_class cls,
                            int level) {
  return oemu_mmu_internal_esr(to_lower_from(cur), is_fetch, is_write,
                               oemu_mmu_internal_dfsc(cls, level));
}

/* One translation through the bus wrapper, with the access kind resolved
 * into the fault record on the way out. */
static oemu_status translate(const oemu_mmu *mmu, uint64_t va, oemu_el cur, bool is_write,
                             bool is_fetch, uint64_t *pa, oemu_mmu_fault *f) {
  walk_fault wf;
  const oemu_status st = walk(mmu->sysregs, &mmu->phys, va, cur, is_write, is_fetch, pa, &wf);
  if (st == OEMU_ERR_FAULT) {
    f->esr = compose_esr(cur, is_fetch, is_write, wf.cls, wf.level);
    f->far = wf.far;
  }
  return st;
}

static oemu_status refuse(oemu_mmu *mmu, oemu_el cur, bool is_fetch, bool is_write,
                          oemu_mmu_fault_class cls, int level, uint64_t far) {
  mmu->fault.esr = compose_esr(cur, is_fetch, is_write, cls, level);
  mmu->fault.far = far;
  mmu->fault_valid = true;
  return OEMU_ERR_FAULT;
}

bool oemu_mmu_take_fault(oemu_mmu *mmu, oemu_mmu_fault *out) {
  OEMU_REQUIRE(mmu != NULL, "NULL oemu_mmu");
  if (!mmu->fault_valid) {
    return false;
  }
  mmu->fault_valid = false;
  if (out != NULL) {
    *out = mmu->fault;
  }
  return true;
}

/* --- public entry points ------------------------------------------------------ */

void oemu_mmu_init(oemu_mmu *mmu, oemu_sysregs *sysregs, const oemu_memops *phys) {
  OEMU_REQUIRE((mmu != NULL) && (sysregs != NULL) && (phys != NULL), "NULL oemu_mmu argument");
  OEMU_REQUIRE(((phys->fetch32 != NULL) && (phys->read != NULL) && (phys->write != NULL) &&
                (phys->validate != NULL)),
               "half-built bus view");
  *mmu = (oemu_mmu){0};
  mmu->sysregs = sysregs;
  mmu->phys = *phys;
}

OEMU_NODISCARD oemu_status oemu_mmu_translate(const oemu_mmu *mmu, uint64_t va, bool is_write,
                                              bool is_fetch, uint64_t *pa_out,
                                              oemu_mmu_fault *fault_out) {
  if ((mmu == NULL) || (pa_out == NULL)) {
    return OEMU_ERR_INVALID_ARG;
  }
  const oemu_el cur = oemu_pstate_el(mmu->sysregs->pstate);
  oemu_mmu_fault discard;
  if (fault_out == NULL) {
    fault_out = &discard;
  }
  return translate(mmu, va, cur, is_write, is_fetch, pa_out, fault_out);
}

/* --- the bus layer -----------------------------------------------------------
 *
 * The order the wrappers apply, matching hardware: the walk first (mapping
 * faults outrank everything), then the alignment policy, then the physical
 * bus's own verdict.
 */

static oemu_status mmu_fetch32(void *ctx, uint64_t va, uint32_t *word_out) {
  oemu_mmu *mmu = ctx;
  const oemu_el cur = oemu_pstate_el(mmu->sysregs->pstate);
  uint64_t pa = 0U;
  oemu_mmu_fault f;
  if (translate(mmu, va, cur, false, true, &pa, &f) != OEMU_OK) {
    mmu->fault = f;
    mmu->fault_valid = true;
    return OEMU_ERR_FAULT;
  }
  /* The bus is the last permission authority: an unmapped or non-executable
   * physical range is a translation fault with no level. */
  if ((mmu->phys.validate(mmu->phys.ctx, pa, 4U, OEMU_PERM_EXEC) != OEMU_OK) ||
      (mmu->phys.fetch32(mmu->phys.ctx, pa, word_out) != OEMU_OK)) {
    return refuse(mmu, cur, true, false, OEMU_MMU_FAULT_TRANSLATION_NO_WALK, -1, va);
  }
  return OEMU_OK;
}

/* Validate every byte of a split access before committing any: the walk may
 * map the halves apart, and a precise layer cannot commit half a store. */
static oemu_status split_check(oemu_mmu *mmu, uint64_t va, unsigned bytes, oemu_el cur,
                               bool is_write, oemu_mmu_fault *f) {
  for (unsigned i = 0U; i < bytes; i++) {
    uint64_t pa = 0U;
    if (translate(mmu, va + i, cur, is_write, false, &pa, f) != OEMU_OK) {
      return OEMU_ERR_FAULT;
    }
    const uint32_t perms = is_write ? OEMU_PERM_WRITE : OEMU_PERM_READ;
    if (mmu->phys.validate(mmu->phys.ctx, pa, 1U, perms) != OEMU_OK) {
      f->esr = compose_esr(cur, false, is_write, OEMU_MMU_FAULT_TRANSLATION_NO_WALK, -1);
      return OEMU_ERR_FAULT;
    }
  }
  return OEMU_OK;
}

static oemu_status mmu_read(void *ctx, uint64_t va, oemu_mem_size size, bool sign_extend,
                            uint64_t *value_out) {
  oemu_mmu *mmu = ctx;
  const oemu_el cur = oemu_pstate_el(mmu->sysregs->pstate);
  const unsigned bytes = 1U << (unsigned)size;
  const bool misaligned = (va & (bytes - 1U)) != 0U;
  uint64_t pa = 0U;
  oemu_mmu_fault f;

  if (translate(mmu, va, cur, false, false, &pa, &f) != OEMU_OK) {
    mmu->fault = f;
    mmu->fault_valid = true;
    return OEMU_ERR_FAULT;
  }
  if (misaligned && oemu_mmu_internal_alignment_required(mmu->sysregs, cur == OEMU_EL0)) {
    return refuse(mmu, cur, false, false, OEMU_MMU_FAULT_ALIGNMENT, -1, va);
  }

  uint64_t value = 0U;
  if (misaligned) {
    if (split_check(mmu, va, bytes, cur, false, &f) != OEMU_OK) {
      mmu->fault = f;
      mmu->fault_valid = true;
      return OEMU_ERR_FAULT;
    }
    for (unsigned i = 0U; i < bytes; i++) {
      uint64_t bpa = 0U;
      (void)translate(mmu, va + i, cur, false, false, &bpa, &f); /* validated above */
      uint64_t byte = 0U;
      if (mmu->phys.read(mmu->phys.ctx, bpa, OEMU_MEM_BYTE, false, &byte) != OEMU_OK) {
        return refuse(mmu, cur, false, false, OEMU_MMU_FAULT_TRANSLATION_NO_WALK, -1, va);
      }
      value |= (byte & 0xFFU) << (i * 8U);
    }
  } else {
    if (mmu->phys.validate(mmu->phys.ctx, pa, bytes, OEMU_PERM_READ) != OEMU_OK) {
      return refuse(mmu, cur, false, false, OEMU_MMU_FAULT_TRANSLATION_NO_WALK, -1, va);
    }
    if (mmu->phys.read(mmu->phys.ctx, pa, size, false, &value) != OEMU_OK) {
      return refuse(mmu, cur, false, false, OEMU_MMU_FAULT_TRANSLATION_NO_WALK, -1, va);
    }
  }
  /* Sign-extend from the access width. bytes is one of {1,2,4,8} from the
   * decoded size; only sub-64-bit widths have a sign bit to extend, so the
   * byte-count test is the whole guard, restated as a shift bound the
   * analyzer can follow. */
  if (sign_extend && (bytes < 8U)) {
    const unsigned sign_bit = bytes * 8U - 1U;
    if ((sign_bit < 64U) && (((value >> sign_bit) & 1U) != 0U)) {
      value |= ~((UINT64_C(1) << (bytes * 8U)) - 1U);
    }
  }
  *value_out = value;
  return OEMU_OK;
}

static oemu_status mmu_write(void *ctx, uint64_t va, oemu_mem_size size, uint64_t value) {
  oemu_mmu *mmu = ctx;
  const oemu_el cur = oemu_pstate_el(mmu->sysregs->pstate);
  const unsigned bytes = 1U << (unsigned)size;
  const bool misaligned = (va & (bytes - 1U)) != 0U;
  uint64_t pa = 0U;
  oemu_mmu_fault f;

  if (translate(mmu, va, cur, true, false, &pa, &f) != OEMU_OK) {
    mmu->fault = f;
    mmu->fault_valid = true;
    return OEMU_ERR_FAULT;
  }
  if (misaligned && oemu_mmu_internal_alignment_required(mmu->sysregs, cur == OEMU_EL0)) {
    return refuse(mmu, cur, false, true, OEMU_MMU_FAULT_ALIGNMENT, -1, va);
  }

  if (misaligned) {
    if (split_check(mmu, va, bytes, cur, true, &f) != OEMU_OK) {
      mmu->fault = f;
      mmu->fault_valid = true;
      return OEMU_ERR_FAULT;
    }
    for (unsigned i = 0U; i < bytes; i++) {
      uint64_t bpa = 0U;
      (void)translate(mmu, va + i, cur, true, false, &bpa, &f); /* validated above */
      if (mmu->phys.write(mmu->phys.ctx, bpa, OEMU_MEM_BYTE, (value >> (i * 8U)) & 0xFFU) !=
          OEMU_OK) {
        return refuse(mmu, cur, false, true, OEMU_MMU_FAULT_TRANSLATION_NO_WALK, -1, va);
      }
    }
    return OEMU_OK;
  }
  if (mmu->phys.validate(mmu->phys.ctx, pa, bytes, OEMU_PERM_WRITE) != OEMU_OK) {
    return refuse(mmu, cur, false, true, OEMU_MMU_FAULT_TRANSLATION_NO_WALK, -1, va);
  }
  if (mmu->phys.write(mmu->phys.ctx, pa, size, value) != OEMU_OK) {
    return refuse(mmu, cur, false, true, OEMU_MMU_FAULT_TRANSLATION_NO_WALK, -1, va);
  }
  return OEMU_OK;
}

/* A permission probe: every page the range spans must translate with the
 * requested permission and be backed. */
static oemu_status mmu_validate(void *ctx, uint64_t va, uint64_t size, uint32_t perms) {
  oemu_mmu *mmu = ctx;
  if (size == 0U) {
    return OEMU_OK;
  }
  const oemu_el cur = oemu_pstate_el(mmu->sysregs->pstate);
  const bool is_fetch = (perms & OEMU_PERM_EXEC) != 0U;
  const bool is_write = (perms & OEMU_PERM_WRITE) != 0U;
  uint64_t off = 0U;
  while (off < size) {
    const uint64_t page_off = (va + off) & (MMU_PAGE_SIZE - 1U);
    const uint64_t span = MMU_PAGE_SIZE - page_off;
    const uint64_t chunk = (span <= (size - off)) ? span : (size - off);
    uint64_t pa = 0U;
    oemu_mmu_fault f;
    if (translate(mmu, va + off, cur, is_write, is_fetch, &pa, &f) != OEMU_OK) {
      mmu->fault = f;
      mmu->fault_valid = true;
      return OEMU_ERR_FAULT;
    }
    if (mmu->phys.validate(mmu->phys.ctx, pa, (uint32_t)chunk, perms) != OEMU_OK) {
      return refuse(mmu, cur, is_fetch, is_write, OEMU_MMU_FAULT_TRANSLATION_NO_WALK, -1,
                    va + off);
    }
    off += chunk;
  }
  return OEMU_OK;
}

oemu_memops oemu_mmu_memops(oemu_mmu *mmu) {
  OEMU_REQUIRE(mmu != NULL, "NULL oemu_mmu");
  return (oemu_memops){mmu, mmu_fetch32, mmu_read, mmu_write, mmu_validate};
}
