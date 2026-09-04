/*
 * Tests for the region-table memory model. The interesting cases are the ones
 * where an implementation could silently get it wrong: containment arithmetic
 * at the very top of the address space, permission checks, and every allocation
 * site on its own failing day.
 */
#include "oemu/memory.h"

#include <array>

#include <gtest/gtest.h>

#include "memory/memory_internal.h"
#include "support/tracking_allocator.h"

namespace {

constexpr uint64_t kBase = UINT64_C(0x10000000);

class MemoryTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_EQ(oemu_memory_init(&mem_, 8U), OEMU_OK); }
  void TearDown() override {
    oemu_memory_dispose(&mem_);
    EXPECT_FALSE(tracker_.has_leaks());
  }

  oemu_memory mem_{};
  oemu_test::TrackingAllocator tracker_;
};

TEST_F(MemoryTest, InitRejectsNullAndZeroCapacity) {
  EXPECT_EQ(oemu_memory_init(nullptr, 4U), OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(oemu_memory_init(&mem_, 0U), OEMU_ERR_INVALID_ARG);
}

TEST_F(MemoryTest, MapRejectsBadArguments) {
  EXPECT_EQ(oemu_memory_map(nullptr, kBase, 0x1000U, OEMU_PERM_ALL), OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(oemu_memory_map(&mem_, kBase, 0U, OEMU_PERM_ALL), OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(oemu_memory_map(&mem_, kBase, 0x1000U, 0U), OEMU_ERR_INVALID_ARG);
}

TEST_F(MemoryTest, MapRejectsWrappingRange) {
  EXPECT_EQ(oemu_memory_map(&mem_, UINT64_MAX - 3U, 8U, OEMU_PERM_ALL), OEMU_ERR_OVERFLOW);
  EXPECT_EQ(oemu_memory_map(&mem_, UINT64_MAX, 1U, OEMU_PERM_ALL), OEMU_OK); /* edge: exact */
}

TEST_F(MemoryTest, MapRejectsOverlapButAllowsAdjacent) {
  ASSERT_EQ(oemu_memory_map(&mem_, kBase, 0x1000U, OEMU_PERM_ALL), OEMU_OK);
  EXPECT_EQ(oemu_memory_map(&mem_, kBase, 0x10U, OEMU_PERM_ALL), OEMU_ERR_RANGE);
  EXPECT_EQ(oemu_memory_map(&mem_, kBase + 0x800U, 0x2000U, OEMU_PERM_ALL), OEMU_ERR_RANGE);
  EXPECT_EQ(oemu_memory_map(&mem_, kBase - 0x800U, 0x800U, OEMU_PERM_ALL), OEMU_OK);
  EXPECT_EQ(oemu_memory_map(&mem_, kBase + 0x1000U, 0x1000U, OEMU_PERM_ALL), OEMU_OK);
  EXPECT_EQ(mem_.region_count, 3U);
}

TEST_F(MemoryTest, MapRejectsWhenTableFull) {
  for (unsigned i = 0U; i < 8U; i++) {
    ASSERT_EQ(oemu_memory_map(&mem_, kBase + (UINT64_C(0x10000) * i), 0x100U, OEMU_PERM_ALL),
              OEMU_OK);
  }
  EXPECT_EQ(oemu_memory_map(&mem_, kBase + UINT64_C(0x1000000), 0x10U, OEMU_PERM_ALL),
            OEMU_ERR_RANGE);
  EXPECT_EQ(mem_.region_count, 8U); /* the rejected map left no trace */
}

TEST_F(MemoryTest, MapBacksRegionWithZeroes) {
  ASSERT_EQ(oemu_memory_map(&mem_, kBase, 0x1000U, OEMU_PERM_READ | OEMU_PERM_WRITE), OEMU_OK);
  uint64_t value = 1U;
  ASSERT_EQ(oemu_memory_read(&mem_, kBase, OEMU_MEM_DWORD, false, &value), OEMU_OK);
  EXPECT_EQ(value, 0U);
  ASSERT_EQ(oemu_memory_read(&mem_, kBase + 0xFFCU, OEMU_MEM_WORD, false, &value), OEMU_OK);
  EXPECT_EQ(value, 0U);
}

TEST_F(MemoryTest, MapImageInstallsContentsUnderReadOnlyPerms) {
  /* The loader's primitive: contents go in at installation time, so a region
   * that the guest may only read still gets populated -- and stays unwritable. */
  const uint8_t payload[4] = {0xDEU, 0xADU, 0xBEU, 0xEFU};
  ASSERT_EQ(oemu_memory_map_image(&mem_, kBase, 0x1000U, OEMU_PERM_READ, payload, 4U), OEMU_OK);
  uint64_t value = 0U;
  ASSERT_EQ(oemu_memory_read(&mem_, kBase, OEMU_MEM_WORD, false, &value), OEMU_OK);
  EXPECT_EQ(value, UINT64_C(0xEFBEADDE)); /* little-endian byte order */
  EXPECT_EQ(oemu_memory_write(&mem_, kBase, OEMU_MEM_WORD, 0U), OEMU_ERR_FAULT);
  /* The .bss tail past the file slice is zero. */
  ASSERT_EQ(oemu_memory_read(&mem_, kBase + 0x800U, OEMU_MEM_DWORD, false, &value), OEMU_OK);
  EXPECT_EQ(value, 0U);
}

TEST_F(MemoryTest, MapImageContentsExactlyFillRegion) {
  const uint8_t payload[8] = {1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U};
  ASSERT_EQ(
      oemu_memory_map_image(&mem_, kBase, 8U, OEMU_PERM_READ | OEMU_PERM_WRITE, payload, 8U),
      OEMU_OK);
  uint64_t value = 0U;
  ASSERT_EQ(oemu_memory_read(&mem_, kBase, OEMU_MEM_DWORD, false, &value), OEMU_OK);
  EXPECT_EQ(value, UINT64_C(0x0807060504030201)); /* {1..8} little-endian */
}

TEST_F(MemoryTest, MapImageRejectsBadContents) {
  const uint8_t payload[4] = {0U};
  /* Contents larger than the region. */
  EXPECT_EQ(oemu_memory_map_image(&mem_, kBase, 4U, OEMU_PERM_READ, payload, 8U),
            OEMU_ERR_INVALID_ARG);
  /* Nonzero contents size with no source pointer. */
  EXPECT_EQ(oemu_memory_map_image(&mem_, kBase, 4U, OEMU_PERM_READ, nullptr, 4U),
            OEMU_ERR_INVALID_ARG);
  /* A null model. */
  EXPECT_EQ(oemu_memory_map_image(nullptr, kBase, 4U, OEMU_PERM_READ, nullptr, 0U),
            OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(mem_.region_count, 0U);
}

TEST_F(MemoryTest, MapImageFailsWhenBackingAllocationFails) {
  oemu_memory_dispose(&mem_);
  {
    const oemu_test::FailingAllocator failing(2U); /* call 1 table, call 2 block */
    const uint8_t payload[4] = {1U};
    ASSERT_EQ(oemu_memory_init(&mem_, 1U), OEMU_OK);
    EXPECT_EQ(oemu_memory_map_image(&mem_, kBase, 0x1000U, OEMU_PERM_ALL, payload, 4U),
              OEMU_ERR_NO_MEMORY);
    EXPECT_EQ(mem_.region_count, 0U);
    oemu_memory_dispose(&mem_);
  }
  ASSERT_EQ(oemu_memory_init(&mem_, 8U), OEMU_OK); /* restore for TearDown */
}

TEST_F(MemoryTest, MapFailsWhenBackingAllocationFails) {
  /* Start from a fresh, minimal table so the failing call lines up exactly:
   * call 1 is the table, call 2 the region's backing block. Dispose before
   * the failing allocator's frame goes away -- the memory object caches the
   * allocator it was initialised with. */
  oemu_memory_dispose(&mem_);
  {
    const oemu_test::FailingAllocator failing(2U);
    ASSERT_EQ(oemu_memory_init(&mem_, 1U), OEMU_OK);
    EXPECT_EQ(oemu_memory_map(&mem_, kBase, 0x1000U, OEMU_PERM_ALL), OEMU_ERR_NO_MEMORY);
    EXPECT_EQ(mem_.region_count, 0U);
    oemu_memory_dispose(&mem_);
  }
  ASSERT_EQ(oemu_memory_init(&mem_, 8U), OEMU_OK); /* restore for TearDown */
}

TEST(MemoryInit, FailsWhenTableAllocationFails) {
  const oemu_test::FailingAllocator failing(1U);
  oemu_memory mem{};
  EXPECT_EQ(oemu_memory_init(&mem, 4U), OEMU_ERR_NO_MEMORY);
}

TEST_F(MemoryTest, FailedMapKeepsNoTraceOfItsTable) {
  /* A rejected backing allocation must leave the table and counts intact:
   * a later map with a working allocator still lands. */
  oemu_memory_dispose(&mem_);
  {
    const oemu_test::FailingAllocator failing(2U);
    ASSERT_EQ(oemu_memory_init(&mem_, 1U), OEMU_OK);
    EXPECT_EQ(oemu_memory_map(&mem_, kBase, 0x1000U, OEMU_PERM_ALL), OEMU_ERR_NO_MEMORY);
    EXPECT_EQ(mem_.region_count, 0U);
    oemu_memory_dispose(&mem_); /* the table was carved from the failing pool */
  }
  ASSERT_EQ(oemu_memory_init(&mem_, 8U), OEMU_OK); /* restore for TearDown */
  ASSERT_EQ(oemu_memory_map(&mem_, kBase, 0x1000U, OEMU_PERM_ALL), OEMU_OK);
  EXPECT_EQ(mem_.region_count, 1U);
}

TEST_F(MemoryTest, AliasIsVisibleBothWays) {
  std::array<uint8_t, 16> host{{0}};
  host[0] = 0xDEU;
  host[1] = 0xADU;
  host[2] = 0xBEU;
  host[3] = 0xEFU;
  ASSERT_EQ(oemu_memory_map_alias(&mem_, kBase, host.data(), host.size(), OEMU_PERM_ALL),
            OEMU_OK);
  uint64_t value = 0U;
  ASSERT_EQ(oemu_memory_read(&mem_, kBase, OEMU_MEM_WORD, false, &value), OEMU_OK);
  EXPECT_EQ(value, UINT64_C(0xEFBEADDE));
  ASSERT_EQ(oemu_memory_write(&mem_, kBase + 8U, OEMU_MEM_DWORD, UINT64_C(0x0807060504030201)),
            OEMU_OK);
  EXPECT_EQ(host[8], 0x01U);
  EXPECT_EQ(host[15], 0x08U);
}

TEST_F(MemoryTest, AliasRejectsNullHost) {
  EXPECT_EQ(oemu_memory_map_alias(&mem_, kBase, nullptr, 16U, OEMU_PERM_ALL),
            OEMU_ERR_INVALID_ARG);
}

TEST_F(MemoryTest, ValidateHonoursBoundsAndPermissions) {
  ASSERT_EQ(oemu_memory_map(&mem_, kBase, 0x1000U, OEMU_PERM_READ), OEMU_OK);
  EXPECT_EQ(oemu_memory_validate(&mem_, kBase, 0x1000U, OEMU_PERM_READ), OEMU_OK);
  EXPECT_EQ(oemu_memory_validate(&mem_, kBase, 0x1001U, OEMU_PERM_READ), OEMU_ERR_FAULT);
  EXPECT_EQ(oemu_memory_validate(&mem_, kBase + 0xFFFU, 1U, OEMU_PERM_READ), OEMU_OK);
  EXPECT_EQ(oemu_memory_validate(&mem_, kBase + 0x1000U, 1U, OEMU_PERM_READ), OEMU_ERR_FAULT);
  EXPECT_EQ(oemu_memory_validate(&mem_, kBase + 1U, 0U, OEMU_PERM_READ), OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(oemu_memory_validate(&mem_, kBase, 1U, OEMU_PERM_WRITE), OEMU_ERR_FAULT);
  EXPECT_EQ(oemu_memory_validate(&mem_, kBase, 1U, OEMU_PERM_READ | OEMU_PERM_WRITE),
            OEMU_ERR_FAULT);
  EXPECT_EQ(oemu_memory_validate(nullptr, kBase, 1U, OEMU_PERM_READ), OEMU_ERR_INVALID_ARG);
}

TEST_F(MemoryTest, AccessAcrossTwoRegionsFaults) {
  ASSERT_EQ(oemu_memory_map(&mem_, kBase, 0x1000U, OEMU_PERM_ALL), OEMU_OK);
  ASSERT_EQ(oemu_memory_map(&mem_, kBase + 0x1000U, 0x1000U, OEMU_PERM_ALL), OEMU_OK);
  /* Contiguous in the guest's eyes, but the model requires one region to
   * contain the whole access. */
  EXPECT_EQ(oemu_memory_validate(&mem_, kBase + 0xFFCU, 8U, OEMU_PERM_ALL), OEMU_ERR_FAULT);
}

TEST_F(MemoryTest, ReadWriteRoundTripsAtEverySize) {
  std::array<uint8_t, 32> host{{0}};
  ASSERT_EQ(oemu_memory_map_alias(&mem_, kBase, host.data(), host.size(), OEMU_PERM_ALL),
            OEMU_OK);
  const oemu_mem_size sizes[4] = {OEMU_MEM_BYTE, OEMU_MEM_HALF, OEMU_MEM_WORD, OEMU_MEM_DWORD};
  for (unsigned i = 0U; i < 4U; i++) {
    const uint64_t pattern =
        (i == 3U) ? UINT64_C(0xDEADBEEF12345678)
                  : (UINT64_C(0xDEADBEEF12345678) & ((UINT64_C(1) << ((i + 1U) * 8U)) - 1U));
    ASSERT_EQ(oemu_memory_write(&mem_, kBase, sizes[i], pattern), OEMU_OK);
    uint64_t value = 0U;
    ASSERT_EQ(oemu_memory_read(&mem_, kBase, sizes[i], false, &value), OEMU_OK);
    EXPECT_EQ(value, pattern);
  }
}

TEST_F(MemoryTest, StoreLittleEndianLayout) {
  std::array<uint8_t, 16> host{{0}};
  ASSERT_EQ(oemu_memory_map_alias(&mem_, kBase, host.data(), host.size(), OEMU_PERM_ALL),
            OEMU_OK);
  ASSERT_EQ(oemu_memory_write(&mem_, kBase, OEMU_MEM_WORD, UINT64_C(0x04030201)), OEMU_OK);
  EXPECT_EQ(host[0], 0x01U);
  EXPECT_EQ(host[1], 0x02U);
  EXPECT_EQ(host[2], 0x03U);
  EXPECT_EQ(host[3], 0x04U);
}

TEST_F(MemoryTest, StoreTruncatesToAccessSize) {
  std::array<uint8_t, 16> host{{0}};
  ASSERT_EQ(oemu_memory_map_alias(&mem_, kBase, host.data(), host.size(), OEMU_PERM_ALL),
            OEMU_OK);
  ASSERT_EQ(oemu_memory_write(&mem_, kBase, OEMU_MEM_HALF, UINT64_C(0xBEEF1234)), OEMU_OK);
  EXPECT_EQ(host[0], 0x34U);
  EXPECT_EQ(host[1], 0x12U);
  EXPECT_EQ(host[2], 0x00U); /* the store must not have spilled */
}

TEST_F(MemoryTest, SignExtensionMatrix) {
  ASSERT_EQ(oemu_memory_map(&mem_, kBase, 0x100U, OEMU_PERM_READ | OEMU_PERM_WRITE), OEMU_OK);
  ASSERT_EQ(oemu_memory_write(&mem_, kBase, OEMU_MEM_DWORD, UINT64_C(0x1234567889ABCDEF)),
            OEMU_OK);
  struct Case {
    oemu_mem_size size;
    bool sign;
    uint64_t expect;
  };
  const Case cases[] = {
      {OEMU_MEM_BYTE, false, UINT64_C(0xEF)},
      {OEMU_MEM_BYTE, true, ~UINT64_C(0x10)},
      {OEMU_MEM_HALF, false, UINT64_C(0xCDEF)},
      {OEMU_MEM_HALF, true, ~UINT64_C(0x3210)},
      {OEMU_MEM_WORD, false, UINT64_C(0x89ABCDEF)},
      {OEMU_MEM_WORD, true, ~UINT64_C(0x76543210)},
      {OEMU_MEM_DWORD, false, UINT64_C(0x1234567889ABCDEF)},
      {OEMU_MEM_DWORD, true, UINT64_C(0x1234567889ABCDEF)},
  };
  for (const Case &c : cases) {
    uint64_t value = 0U;
    ASSERT_EQ(oemu_memory_read(&mem_, kBase, c.size, c.sign, &value), OEMU_OK)
        << "size=" << c.size << " sign=" << c.sign;
    EXPECT_EQ(value, c.expect) << "size=" << c.size << " sign=" << c.sign;
  }
}

TEST_F(MemoryTest, ReadRejectsNullOutput) {
  ASSERT_EQ(oemu_memory_map(&mem_, kBase, 0x100U, OEMU_PERM_READ), OEMU_OK);
  EXPECT_EQ(oemu_memory_read(&mem_, kBase, OEMU_MEM_WORD, false, nullptr),
            OEMU_ERR_INVALID_ARG);
}

TEST_F(MemoryTest, WriteBytesCopiesWholeRangeOrNothing) {
  std::array<uint8_t, 8> host{{0}};
  ASSERT_EQ(oemu_memory_map_alias(&mem_, kBase, host.data(), host.size(), OEMU_PERM_WRITE),
            OEMU_OK);
  const std::array<uint8_t, 6> src{{1, 2, 3, 4, 5, 6}};
  ASSERT_EQ(oemu_memory_write_bytes(&mem_, kBase, src.data(), 6U), OEMU_OK);
  EXPECT_EQ(host[5], 6U);

  /* Runs off the end: the whole validation happens first, so the earlier
   * bytes must still hold the previous contents. */
  EXPECT_EQ(oemu_memory_write_bytes(&mem_, kBase + 4U, src.data(), 8U), OEMU_ERR_FAULT);
  EXPECT_EQ(host[4], 5U); /* the earlier successful write's byte, untouched */
  EXPECT_EQ(host[5], 6U);

  EXPECT_EQ(oemu_memory_write_bytes(&mem_, kBase, src.data(), 0U), OEMU_OK);
  EXPECT_EQ(oemu_memory_write_bytes(&mem_, kBase, nullptr, 4U), OEMU_ERR_INVALID_ARG);
}

TEST_F(MemoryTest, Fetch32ChecksAlignmentAndExecutability) {
  std::array<uint8_t, 8> host{{0x78, 0x56, 0x34, 0x12, 0, 0, 0, 0}};
  ASSERT_EQ(oemu_memory_map_alias(&mem_, kBase, host.data(), host.size(),
                                  OEMU_PERM_READ | OEMU_PERM_EXEC),
            OEMU_OK);
  uint32_t word = 0U;
  ASSERT_EQ(oemu_memory_fetch32(&mem_, kBase, &word), OEMU_OK);
  EXPECT_EQ(word, 0x12345678U);
  EXPECT_EQ(oemu_memory_fetch32(&mem_, kBase + 1U, &word), OEMU_ERR_FAULT);
  EXPECT_EQ(oemu_memory_fetch32(&mem_, kBase + 6U, &word), OEMU_ERR_FAULT); /* straddles end */
  EXPECT_EQ(oemu_memory_fetch32(&mem_, kBase, nullptr), OEMU_ERR_INVALID_ARG);

  /* A writable-but-not-executable region cannot be executed from. */
  ASSERT_EQ(oemu_memory_map(&mem_, kBase + 0x1000U, 0x100U, OEMU_PERM_READ | OEMU_PERM_WRITE),
            OEMU_OK);
  EXPECT_EQ(oemu_memory_fetch32(&mem_, kBase + 0x1000U, &word), OEMU_ERR_FAULT);
}

TEST_F(MemoryTest, TopOfAddressSpaceIsMappableAndAccessible) {
  /* The trap this model could easily have: containing a range is tested with
   * subtractions precisely so that [2^64-page, 2^64) is a legal mapping. */
  std::array<uint8_t, 4> host{{1, 2, 3, 4}};
  ASSERT_EQ(oemu_memory_map_alias(&mem_, UINT64_C(0xFFFFFFFFFFFFFFFC), host.data(), host.size(),
                                  OEMU_PERM_ALL),
            OEMU_OK);
  uint64_t value = 0U;
  ASSERT_EQ(oemu_memory_read(&mem_, UINT64_C(0xFFFFFFFFFFFFFFFC), OEMU_MEM_WORD, false, &value),
            OEMU_OK);
  EXPECT_EQ(value, UINT64_C(0x04030201));
  EXPECT_EQ(oemu_memory_validate(&mem_, UINT64_C(0xFFFFFFFFFFFFFFFC), 5U, OEMU_PERM_READ),
            OEMU_ERR_FAULT);
}

TEST_F(MemoryTest, DisposeIsIdempotentAndAllowsReinit) {
  ASSERT_EQ(oemu_memory_map(&mem_, kBase, 0x100U, OEMU_PERM_ALL), OEMU_OK);
  oemu_memory_dispose(&mem_);
  oemu_memory_dispose(&mem_); /* second dispose must be harmless */
  ASSERT_EQ(oemu_memory_init(&mem_, 2U), OEMU_OK);
  ASSERT_EQ(oemu_memory_map(&mem_, kBase, 0x40U, OEMU_PERM_ALL), OEMU_OK);
  /* A disposed model must refuse maps rather than dereference NULL. */
  oemu_memory_dispose(&mem_);
  EXPECT_EQ(oemu_memory_map(&mem_, kBase, 0x40U, OEMU_PERM_ALL), OEMU_ERR_INVALID_ARG);
  ASSERT_EQ(oemu_memory_init(&mem_, 8U), OEMU_OK); /* so TearDown stays happy */
}

TEST(InternalHelpers, OverlapIsHalfOpenAndTouchingIsNotOverlap) {
  EXPECT_FALSE(oemu_memory_internal_ranges_overlap(0U, 0x10U, 0x10U, 0x10U));
  EXPECT_FALSE(oemu_memory_internal_ranges_overlap(0x20U, 0x10U, 0x10U, 0x10U));
  EXPECT_TRUE(oemu_memory_internal_ranges_overlap(0U, 0x11U, 0x10U, 0x10U));
  EXPECT_FALSE(oemu_memory_internal_ranges_overlap(0xFU, 1U, 0x10U, 0x10U));
  EXPECT_TRUE(oemu_memory_internal_ranges_overlap(0xFU, 2U, 0x10U, 0x10U));
  EXPECT_TRUE(oemu_memory_internal_ranges_overlap(0x10U, 0x10U, 0x10U, 0x10U));
  EXPECT_FALSE(oemu_memory_internal_ranges_overlap(0U, 0U, 0U, 0U));
}

TEST(InternalHelpers, CheckMapArgsCoversEveryRejection) {
  EXPECT_EQ(oemu_memory_internal_check_map_args(0x1000U, 0x10U, OEMU_PERM_READ), OEMU_OK);
  EXPECT_EQ(oemu_memory_internal_check_map_args(0x1000U, 0U, OEMU_PERM_READ),
            OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(oemu_memory_internal_check_map_args(0x1000U, 0x10U, 0U), OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(
      oemu_memory_internal_check_map_args(UINT64_C(0xFFFFFFFFFFFFFFFF), 2U, OEMU_PERM_READ),
      OEMU_ERR_OVERFLOW);
  EXPECT_EQ(oemu_memory_internal_check_map_args(0U, UINT64_MAX, OEMU_PERM_READ), OEMU_OK);
}

TEST(InternalHelpers, AssembleDisassembleAreLittleEndian) {
  const std::array<uint8_t, 8> bytes{{9, 8, 7, 6, 5, 4, 3, 2}};
  EXPECT_EQ(oemu_memory_internal_disassemble(bytes.data(), 1U), UINT64_C(9));
  EXPECT_EQ(oemu_memory_internal_disassemble(bytes.data(), 2U), UINT64_C(0x0809));
  EXPECT_EQ(oemu_memory_internal_disassemble(bytes.data(), 4U), UINT64_C(0x06070809));
  EXPECT_EQ(oemu_memory_internal_disassemble(bytes.data(), 8U), UINT64_C(0x0203040506070809));
  std::array<uint8_t, 8> out{{0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA, 0xAA}};
  oemu_memory_internal_assemble(out.data(), 3U, UINT64_C(0x221100));
  EXPECT_EQ(out[0], 0x00U);
  EXPECT_EQ(out[1], 0x11U);
  EXPECT_EQ(out[2], 0x22U);
  EXPECT_EQ(out[3], 0xAAU); /* untouched */
  for (unsigned n = 1U; n <= 8U; n++) {
    std::array<uint8_t, 8> scratch{{0}};
    const uint64_t value = (n == 8U) ? UINT64_C(0xFEDCBA9876543210)
                                     : (UINT64_C(0x8877665544332211) >> (64U - n * 8U));
    oemu_memory_internal_assemble(scratch.data(), n, value);
    EXPECT_EQ(oemu_memory_internal_disassemble(scratch.data(), n), value);
  }
}

TEST(InternalHelpers, FindRejectsNullModelAndEmptyRange) {
  EXPECT_EQ(oemu_memory_internal_find(nullptr, 0U, 1U, OEMU_PERM_READ), nullptr);
  oemu_memory mem{};
  ASSERT_EQ(oemu_memory_init(&mem, 2U), OEMU_OK);
  ASSERT_EQ(oemu_memory_map(&mem, kBase, 0x100U, OEMU_PERM_ALL), OEMU_OK);
  EXPECT_EQ(oemu_memory_internal_find(&mem, kBase, 0U, OEMU_PERM_ALL), nullptr);
  EXPECT_NE(oemu_memory_internal_find(&mem, kBase + 0x80U, 0x40U, OEMU_PERM_WRITE), nullptr);
  oemu_memory_dispose(&mem);
}

}  // namespace
