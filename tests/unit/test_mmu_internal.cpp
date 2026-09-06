/*
 * White-box tests for the mmu decision functions: the DFSC table, the
 * geometry arithmetic, the permission matrix, and the syndrome
 * composition. These pin the numbers the ARM ARM and Linux's asm/esr.h
 * agree on, so a refactor of the walk cannot move a fault code.
 *
 * Reaches into src/mmu/mmu_internal.h.
 */
#include "oemu/sysreg.h"

#include <cstdint>

#include <gtest/gtest.h>

#include "mmu/mmu_internal.h"

namespace {

constexpr uint32_t kIlBit = UINT32_C(1) << 25;

constexpr uint32_t EcBase(uint32_t ec) {
  return ec << 26U;
}

/* EC values as the exception module names them. */
constexpr uint32_t kIAbortLower = 0x20U;
constexpr uint32_t kIAbortSame = 0x21U;
constexpr uint32_t kDAbortLower = 0x24U;
constexpr uint32_t kDAbortSame = 0x25U;

// --- DFSC table (Linux arch/arm64/include/asm/esr.h: FSC_FAULT_L,
// FSC_ACCESS_L, FSC_PERM_L, FSC_ADDRSZ_L, FSC_FAULT_nL, alignment 0x21). ---

TEST(MmuDfsc, TranslationPerLevel) {
  EXPECT_EQ(oemu_mmu_internal_dfsc(OEMU_MMU_FAULT_TRANSLATION, 0), 0x04U);
  EXPECT_EQ(oemu_mmu_internal_dfsc(OEMU_MMU_FAULT_TRANSLATION, 1), 0x05U);
  EXPECT_EQ(oemu_mmu_internal_dfsc(OEMU_MMU_FAULT_TRANSLATION, 2), 0x06U);
  EXPECT_EQ(oemu_mmu_internal_dfsc(OEMU_MMU_FAULT_TRANSLATION, 3), 0x07U);
}

TEST(MmuDfsc, NoWalkTranslationIs2C) {
  EXPECT_EQ(oemu_mmu_internal_dfsc(OEMU_MMU_FAULT_TRANSLATION_NO_WALK, -1), 0x2CU);
}

TEST(MmuDfsc, AccessFlagAndPermissionAreLevelTagged) {
  EXPECT_EQ(oemu_mmu_internal_dfsc(OEMU_MMU_FAULT_ACCESS_FLAG, 1), 0x09U);
  EXPECT_EQ(oemu_mmu_internal_dfsc(OEMU_MMU_FAULT_ACCESS_FLAG, 2), 0x0AU);
  EXPECT_EQ(oemu_mmu_internal_dfsc(OEMU_MMU_FAULT_ACCESS_FLAG, 3), 0x0BU);
  EXPECT_EQ(oemu_mmu_internal_dfsc(OEMU_MMU_FAULT_PERMISSION, 1), 0x0DU);
  EXPECT_EQ(oemu_mmu_internal_dfsc(OEMU_MMU_FAULT_PERMISSION, 2), 0x0EU);
  EXPECT_EQ(oemu_mmu_internal_dfsc(OEMU_MMU_FAULT_PERMISSION, 3), 0x0FU);
}

TEST(MmuDfsc, AddressSizeHasThreeShapes) {
  EXPECT_EQ(oemu_mmu_internal_dfsc(OEMU_MMU_FAULT_ADDRESS_SIZE, -1), 0x29U); /* TTBR base */
  EXPECT_EQ(oemu_mmu_internal_dfsc(OEMU_MMU_FAULT_ADDRESS_SIZE, -2), 0x2CU); /* other deep */
  EXPECT_EQ(oemu_mmu_internal_dfsc(OEMU_MMU_FAULT_ADDRESS_SIZE, 0), 0x00U);
  EXPECT_EQ(oemu_mmu_internal_dfsc(OEMU_MMU_FAULT_ADDRESS_SIZE, 3), 0x03U);
}

TEST(MmuDfsc, AlignmentIs21) {
  EXPECT_EQ(oemu_mmu_internal_dfsc(OEMU_MMU_FAULT_ALIGNMENT, -1), 0x21U);
}

// --- geometry ---------------------------------------------------------------

TEST(MmuGeometry, StartLevelForWidths) {
  EXPECT_EQ(oemu_mmu_internal_start_level(48), 0);
  EXPECT_EQ(oemu_mmu_internal_start_level(39), 1); /* the tinyconfig default */
  EXPECT_EQ(oemu_mmu_internal_start_level(31), 1);
  EXPECT_EQ(oemu_mmu_internal_start_level(30), 2);
  EXPECT_EQ(oemu_mmu_internal_start_level(25), 2);
}

TEST(MmuGeometry, UnsupportedWidthsReturnMinusOne) {
  EXPECT_EQ(oemu_mmu_internal_start_level(24), -1); /* TnSZ 40: no 4K start */
  EXPECT_EQ(oemu_mmu_internal_start_level(49), -1); /* TnSZ 15: past the 48-bit map */
}

TEST(MmuGeometry, RegionSelectIsBit55) {
  EXPECT_FALSE(oemu_mmu_internal_use_ttbr1(0U));
  EXPECT_FALSE(oemu_mmu_internal_use_ttbr1((UINT64_C(1) << 55U) - 1U));
  EXPECT_TRUE(oemu_mmu_internal_use_ttbr1(UINT64_C(1) << 55U));
  EXPECT_TRUE(oemu_mmu_internal_use_ttbr1(UINT64_C(0xFFFF000000000000)));
}

TEST(MmuGeometry, LowerRegionTopBitsMustBeZero) {
  EXPECT_TRUE(oemu_mmu_internal_region_ok(0x1234ULL, 39U, 64U, false));
  EXPECT_TRUE(oemu_mmu_internal_region_ok((UINT64_C(1) << 38U), 39U, 64U, false));
  EXPECT_FALSE(oemu_mmu_internal_region_ok(UINT64_C(1) << 39U, 39U, 64U, false));
}

TEST(MmuGeometry, UpperRegionTopBitsMustBeOnes) {
  EXPECT_TRUE(oemu_mmu_internal_region_ok(~UINT64_C(0), 39U, 64U, true));
  EXPECT_TRUE(oemu_mmu_internal_region_ok(UINT64_C(0xFFFFFF8000000000), 39U, 64U, true));
  EXPECT_FALSE(oemu_mmu_internal_region_ok(UINT64_C(0x00001F8000000000), 39U, 64U, true));
}

TEST(MmuGeometry, TopByteIgnoreStripsTheTagFromTheCheck) {
  /* TBI: bits [63:56] are not part of the address and not part of the sign
   * check -- a tagged address is judged on [55:inputsize] alone. */
  const uint64_t tagged = (UINT64_C(0xA5) << 56U) | 0x1234ULL;
  EXPECT_TRUE(oemu_mmu_internal_region_ok(tagged, 39U, 56U, false));
  EXPECT_FALSE(oemu_mmu_internal_region_ok(tagged, 39U, 64U, false)); /* no TBI: fails */
  const uint64_t tagged_upper = (UINT64_C(0x5A) << 56U) | UINT64_C(0xFFFFFF8000000000);
  EXPECT_TRUE(oemu_mmu_internal_region_ok(tagged_upper, 39U, 56U, true));
}

TEST(MmuGeometry, FullWidthGeometryStillSignExtends) {
  /* 48-bit input at the widest: the gap is still there, between bit 47's
   * extension and bit 55's. */
  EXPECT_TRUE(oemu_mmu_internal_region_ok(UINT64_C(0xFFFF000000001234), 48U, 64U, true));
  EXPECT_FALSE(oemu_mmu_internal_region_ok(UINT64_C(1) << 63U, 48U, 64U, true));
}

TEST(MmuGeometry, IndexShiftsPerLevel) {
  const uint64_t va = (UINT64_C(0x1FF) << 12U) | 0x1234ULL; /* level-3 index all ones */
  EXPECT_EQ(oemu_mmu_internal_index(va, 3U), 0xFF8U);       /* 511 entries * 8 bytes */
  EXPECT_EQ(oemu_mmu_internal_index(va, 2U), 0U);
  const uint64_t wide = UINT64_C(0x1FF) << 39U;
  EXPECT_EQ(oemu_mmu_internal_index(wide, 0U), 0xFF8U);
}

// --- policy -------------------------------------------------------------------

TEST(MmuAlignment, El1FollowsAAndEl0FollowsSa0) {
  oemu_sysregs sr{};
  sr.sctlr_el1 = 0U;
  EXPECT_FALSE(oemu_mmu_internal_alignment_required(&sr, false));
  EXPECT_FALSE(oemu_mmu_internal_alignment_required(&sr, true));
  sr.sctlr_el1 = UINT64_C(1) << 1; /* A */
  EXPECT_TRUE(oemu_mmu_internal_alignment_required(&sr, false));
  EXPECT_FALSE(oemu_mmu_internal_alignment_required(&sr, true));
  sr.sctlr_el1 = UINT64_C(1) << 4; /* SA0 */
  EXPECT_FALSE(oemu_mmu_internal_alignment_required(&sr, false));
  EXPECT_TRUE(oemu_mmu_internal_alignment_required(&sr, true));
  sr.sctlr_el1 = (UINT64_C(1) << 1) | (UINT64_C(1) << 4);
  EXPECT_TRUE(oemu_mmu_internal_alignment_required(&sr, false));
  EXPECT_TRUE(oemu_mmu_internal_alignment_required(&sr, true));
}

// The permission matrix, ARM ARM D8 cross-checked against QEMU's get_S1prot.
struct ApCase {
  unsigned ap;
  bool el1_read, el1_write, el0_read, el0_write;
};

class MmuPermits : public ::testing::TestWithParam<ApCase> {};

TEST_P(MmuPermits, MatchesTheArchitecturalTable) {
  const ApCase &c = GetParam();
  EXPECT_EQ(oemu_mmu_internal_permits(c.ap, false, false, false, false, false), c.el1_read);
  EXPECT_EQ(oemu_mmu_internal_permits(c.ap, false, false, false, true, false), c.el1_write);
  EXPECT_EQ(oemu_mmu_internal_permits(c.ap, false, false, true, false, false), c.el0_read);
  EXPECT_EQ(oemu_mmu_internal_permits(c.ap, false, false, true, true, false), c.el0_write);
}

INSTANTIATE_TEST_SUITE_P(ApMatrix, MmuPermits,
                         ::testing::Values(ApCase{0b00, true, true, false, false},
                                           ApCase{0b01, true, true, true, true},
                                           ApCase{0b10, true, false, false, false},
                                           ApCase{0b11, true, false, true, false}));

TEST(MmuPermits, XnBlocksOnlyEl0ExecutionAndPxnOnlyEl1) {
  EXPECT_TRUE(oemu_mmu_internal_permits(0b00, false, false, false, false, true)); /* EL1 exec */
  EXPECT_FALSE(oemu_mmu_internal_permits(0b00, false, true, false, false, true)); /* PXN */
  EXPECT_TRUE(oemu_mmu_internal_permits(0b00, true, false, false, false,
                                        true)); /* XN is not EL1's gate */
  EXPECT_FALSE(
      oemu_mmu_internal_permits(0b01, true, false, true, false, true)); /* XN blocks EL0 */
  EXPECT_TRUE(oemu_mmu_internal_permits(0b01, false, true, true, false,
                                        true)); /* PXN is not EL0's gate */
  /* EL0 execution needs the user bit even without XN. */
  EXPECT_FALSE(oemu_mmu_internal_permits(0b00, false, false, true, false, true));
}

TEST(MmuPermits, DataAbortsIgnoreTheExecuteBits) {
  EXPECT_TRUE(oemu_mmu_internal_permits(0b00, true, true, false, false, false));
  EXPECT_TRUE(oemu_mmu_internal_permits(0b00, true, true, false, true, false));
}

// --- syndrome composition -------------------------------------------------------

TEST(MmuEsr, FetchTakesTheInstructionAbortClasses) {
  EXPECT_EQ(oemu_mmu_internal_esr(false, true, false, 0x04U),
            EcBase(kIAbortSame) | kIlBit | 0x04U);
  EXPECT_EQ(oemu_mmu_internal_esr(true, true, false, 0x07U),
            EcBase(kIAbortLower) | kIlBit | 0x07U);
}

TEST(MmuEsr, DataTakesTheDataAbortClassesWithWnr) {
  EXPECT_EQ(oemu_mmu_internal_esr(false, false, true, 0x0EU),
            EcBase(kDAbortSame) | kIlBit | (1U << 6) | 0x0EU);
  EXPECT_EQ(oemu_mmu_internal_esr(true, false, false, 0x2CU),
            EcBase(kDAbortLower) | kIlBit | 0x2CU);
}

TEST(MmuEsr, InstructionAbortsNeverCarryWnr) {
  /* The fetch ISS has no WnR field at all: even a mislabelled write must not
   * leak the bit into an instruction abort. */
  EXPECT_EQ(oemu_mmu_internal_esr(false, true, true, 0x04U),
            EcBase(kIAbortSame) | kIlBit | 0x04U);
}

TEST(MmuEsr, IsvStaysClear) {
  EXPECT_EQ(oemu_mmu_internal_esr(true, false, true, 0x21U) & (1U << 24), 0U);
}

// --- descriptor decoding ---------------------------------------------------------

TEST(MmuDecode, BlockDescriptorFields) {
  const uint64_t d = UINT64_C(1)                 /* valid */
                     | (UINT64_C(0x8123) << 12U) /* output address */
                     | (UINT64_C(1) << 10U)      /* AF */
                     | (UINT64_C(1) << 7U)       /* AP[2] */
                     | (UINT64_C(1) << 6U)       /* AP[1] */
                     | (UINT64_C(1) << 54U);     /* XN */
  const oemu_mmu_desc r = oemu_mmu_internal_decode(d);
  EXPECT_TRUE(r.valid);
  EXPECT_FALSE(r.table);
  EXPECT_TRUE(r.block_page);
  EXPECT_EQ(r.address, UINT64_C(0x8123000));
  EXPECT_TRUE(r.af);
  EXPECT_EQ(r.ap, 0b11U);
  EXPECT_TRUE(r.xn);
  EXPECT_FALSE(r.pxn);
  EXPECT_FALSE(r.table_xn);
}

TEST(MmuDecode, TableDescriptorIsType11AndCarriesItsConstraints) {
  const uint64_t d = UINT64_C(3) | (UINT64_C(1) << 59U) /* PXN */
                     | (UINT64_C(1) << 60U)             /* XN */
                     | (UINT64_C(1) << 61U)             /* APTable[0] */
                     | (UINT64_C(1) << 62U);            /* APTable[1] */
  const oemu_mmu_desc r = oemu_mmu_internal_decode(d);
  EXPECT_TRUE(r.table);
  EXPECT_FALSE(r.block_page);
  EXPECT_TRUE(r.table_pxn);
  EXPECT_TRUE(r.table_xn);
  EXPECT_TRUE(r.table_apt0);
  EXPECT_TRUE(r.table_apt1);
  EXPECT_EQ(oemu_mmu_internal_table_attrs(d), 0x0FU); /* PXN|XN|APT0|APT1 */
}

TEST(MmuDecode, InvalidDescriptorIsOnlyBit0Clear) {
  EXPECT_FALSE(oemu_mmu_internal_decode(UINT64_C(0)).valid);
  EXPECT_FALSE(oemu_mmu_internal_decode(UINT64_C(2)).valid); /* reserved 0b10 */
  EXPECT_FALSE(oemu_mmu_internal_decode(UINT64_C(2)).table); /* a table is type 0b11 */
  EXPECT_FALSE(oemu_mmu_internal_decode(UINT64_C(2)).block_page);
}

TEST(MmuDecode, AddressFieldEndsBelowBit48) {
  const uint64_t d = UINT64_C(1) | (UINT64_C(0xFFFFFFFFFFF) << 12U) | (UINT64_C(0xF) << 48U);
  /* The input sets bits [12:55]; the field keeps [47:12] and nothing above. */
  EXPECT_EQ(oemu_mmu_internal_decode(d).address, UINT64_C(0x0000FFFFFFFFF000));
}

TEST(MmuDecode, TableAttrsAreTheTopFiveBits) {
  EXPECT_EQ(oemu_mmu_internal_table_attrs(UINT64_C(0xF8) << 56U), 0x1FU); /* bits 59..63 */
  EXPECT_EQ(oemu_mmu_internal_table_attrs((UINT64_C(1) << 12U) | UINT64_C(1)), 0U);
}

}  // namespace
