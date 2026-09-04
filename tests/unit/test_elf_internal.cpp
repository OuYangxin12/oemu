/*
 * White-box tests for the ELF loader's pure primitives.
 *
 * These are the functions whose bugs would be silent in the black-box tests: a
 * byte-order slip on a big-endian host, an off-by-one at a bounds edge, a wrap
 * the whole-table guard was supposed to catch. Reaching them directly lets each
 * be driven at its exact boundary.
 */
#include "oemu/memory.h"
#include "oemu/status.h"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "elf/elf_internal.h"

namespace {

TEST(ElfInternalWithin, AcceptsSpansThatFitAndRejectsThoseThatDoNot) {
  EXPECT_TRUE(oemu_elf_internal_within(0U, 0U, 0U));
  EXPECT_TRUE(oemu_elf_internal_within(0U, 64U, 64U));  /* exact fit */
  EXPECT_TRUE(oemu_elf_internal_within(64U, 0U, 64U));  /* empty span at the end */
  EXPECT_FALSE(oemu_elf_internal_within(64U, 1U, 64U)); /* one past the end */
  EXPECT_FALSE(oemu_elf_internal_within(65U, 0U, 64U)); /* off beyond size */
}

TEST(ElfInternalWithin, HugeSpanCannotWrapTheCheck) {
  /* off+span would wrap, but the difference form rejects it. */
  EXPECT_FALSE(oemu_elf_internal_within(16U, UINT64_MAX, 64U));
  EXPECT_TRUE(oemu_elf_internal_within(0U, UINT64_MAX, 0U) == false);
}

TEST(ElfInternalReaders, ReadLittleEndianAtOddOffsets) {
  const uint8_t img[] = {0x00U, 0xEFU, 0xBEU, 0xADU, 0xDEU, 0x00U, 0x00U, 0x00U, 0x00U};
  uint16_t v16 = 0U;
  uint32_t v32 = 0U;
  uint64_t v64 = 0U;
  /* A u32 at offset 1 (deliberately unaligned). */
  ASSERT_EQ(oemu_elf_internal_rd32(img, sizeof(img), 1U, &v32), OEMU_OK);
  EXPECT_EQ(v32, 0xDEADBEEFU);
  ASSERT_EQ(oemu_elf_internal_rd16(img, sizeof(img), 1U, &v16), OEMU_OK);
  EXPECT_EQ(v16, 0xBEEFU);
  ASSERT_EQ(oemu_elf_internal_rd64(img, sizeof(img), 1U, &v64), OEMU_OK);
  EXPECT_EQ(v64, UINT64_C(0x00000000DEADBEEF));
}

TEST(ElfInternalReaders, RejectOutOfBoundAndNull) {
  const uint8_t img[] = {0x01U, 0x02U, 0x03U, 0x04U};
  uint32_t v32 = 0U;
  uint16_t v16 = 0U;
  uint64_t v64 = 0U;
  /* A 4-byte read that would run one past a 4-byte image. */
  EXPECT_EQ(oemu_elf_internal_rd32(img, 4U, 1U, &v32), OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(oemu_elf_internal_rd16(img, 4U, 4U, &v16), OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(oemu_elf_internal_rd64(img, 8U, 1U, &v64), OEMU_ERR_INVALID_ARG);
  /* Null image and null out are both rejected before any read. */
  EXPECT_EQ(oemu_elf_internal_rd32(nullptr, 16U, 0U, &v32), OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(oemu_elf_internal_rd32(img, 4U, 0U, nullptr), OEMU_ERR_INVALID_ARG);
}

TEST(ElfInternalReaders, TopOfSpaceIsRejectedNotWrapped) {
  /* An offset near SIZE_MAX would make off+8 wrap; the difference-form bounds
   * test must reject it (INVALID_ARG) rather than wrap and read out of bounds.
   * A read at a genuinely valid offset on the same real buffer still works. */
  const uint8_t img[16] = {0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U, 0x88U,
                           0x99U, 0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU, 0xFFU, 0x00U};
  uint64_t v64 = 0U;
  uint32_t v32 = 0U;
  EXPECT_EQ(oemu_elf_internal_rd64(img, 16U, UINT64_C(0xFFFFFFFFFFFFFFF8), &v64),
            OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(oemu_elf_internal_rd32(img, 16U, UINT64_MAX, &v32), OEMU_ERR_INVALID_ARG);
  /* Same buffer: the last valid 8-byte read is at offset 8. */
  ASSERT_EQ(oemu_elf_internal_rd64(img, 16U, 8U, &v64), OEMU_OK);
  EXPECT_EQ(v64, UINT64_C(0x00FFEEDDCCBBAA99));
}

TEST(ElfInternalOverlap, HalfOpenRanges) {
  EXPECT_TRUE(oemu_elf_internal_ranges_overlap(0U, 100U, 50U, 100U));   /* partial */
  EXPECT_TRUE(oemu_elf_internal_ranges_overlap(0U, 100U, 10U, 10U));    /* containment */
  EXPECT_FALSE(oemu_elf_internal_ranges_overlap(0U, 100U, 100U, 100U)); /* adjacent */
  EXPECT_FALSE(oemu_elf_internal_ranges_overlap(0U, 100U, 200U, 100U)); /* disjoint */
  EXPECT_FALSE(oemu_elf_internal_ranges_overlap(0U, 0U, 0U, 100U)); /* empty never overlaps */
}

class ElfValidate : public ::testing::Test {
 protected:
  oemu_elf_segment seg_{};
};

TEST_F(ElfValidate, AcceptsAWellFormedSegment) {
  ASSERT_EQ(oemu_elf_internal_validate_segment(0x5U /* RX */, 0x100U, 0x400000U, 4U, 0x1000U,
                                               0x1000U, 0x10000U, &seg_),
            OEMU_OK);
  EXPECT_EQ(seg_.vaddr, 0x400000U);
  EXPECT_EQ(seg_.memsz, 0x1000U);
  EXPECT_EQ(seg_.filesz, 4U);
  EXPECT_EQ(seg_.offset, 0x100U);
  EXPECT_EQ(seg_.perms, OEMU_PERM_READ | OEMU_PERM_EXEC);
}

TEST_F(ElfValidate, MapsAllThreePermissionBits) {
  ASSERT_EQ(oemu_elf_internal_validate_segment(0x7U /* RWX */, 0U, 0x800000U, 0U, 0x1000U, 1U,
                                               0x10000U, &seg_),
            OEMU_OK);
  EXPECT_EQ(seg_.perms, OEMU_PERM_READ | OEMU_PERM_WRITE | OEMU_PERM_EXEC);
}

TEST_F(ElfValidate, ZeroAlignNoPermsAndZeroMemszAreInvalid) {
  EXPECT_EQ(
      oemu_elf_internal_validate_segment(0x5U, 0U, 0x1000U, 0U, 0x1000U, 0U, 0x100U, &seg_),
      OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(
      oemu_elf_internal_validate_segment(0x0U, 0U, 0x1000U, 0U, 0x1000U, 1U, 0x100U, &seg_),
      OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(oemu_elf_internal_validate_segment(0x5U, 0U, 0x1000U, 0U, 0U, 1U, 0x100U, &seg_),
            OEMU_ERR_INVALID_ARG);
}

TEST_F(ElfValidate, FileszOverMemszOrPastImageIsInvalid) {
  EXPECT_EQ(oemu_elf_internal_validate_segment(0x5U, 0U, 0x1000U, 8U, 4U, 1U, 0x100U, &seg_),
            OEMU_ERR_INVALID_ARG); /* filesz 8 > memsz 4 */
  /* offset+filesz runs past a small image. */
  EXPECT_EQ(oemu_elf_internal_validate_segment(0x5U, 0xF0U, 0x1000U, 0x20U, 0x100U, 1U, 0x100U,
                                               &seg_),
            OEMU_ERR_INVALID_ARG);
}

TEST_F(ElfValidate, VaddrWrapIsOverflow) {
  EXPECT_EQ(oemu_elf_internal_validate_segment(0x5U, 0U, UINT64_C(0xFFFFFFFFFFFFFFFF), 0U,
                                               0x1000U, 1U, 0x10000U, &seg_),
            OEMU_ERR_OVERFLOW);
}

TEST_F(ElfValidate, NullOutIsInvalid) {
  EXPECT_EQ(
      oemu_elf_internal_validate_segment(0x5U, 0U, 0x1000U, 0U, 0x1000U, 1U, 0x100U, nullptr),
      OEMU_ERR_INVALID_ARG);
}

}  // namespace
