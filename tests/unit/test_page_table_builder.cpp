/*
 * Pins the page-table builder's encodings and mapping placement -- the oracle
 * foundation every future M3a walker test stands on. If a constant here is
 * wrong, every MMU test built on the builder inherits the error, so the
 * expected values are hand-computed literals (derivation shown inline) checked
 * against two external references: Linux 6.6 arch/arm64/include/asm/
 * pgtable-hwdef.h (the bit positions, quoted in page_table_builder.h) and the
 * TnSZ start-level thresholds derived from the 4 KiB granule geometry
 * ((4-L)*9+12 covered VA bits; tinyconfig's T0SZ=25 -> L1 root is the
 * empirical anchor). Nothing in this file compares the builder against itself.
 */
#include "oemu/aspace.h"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "support/page_table_builder.h"
#include "support/tracking_allocator.h"

namespace {

using namespace oemu_test;

/* --- geometry: TnSZ, VA width, indices ------------------------------------- */

TEST(PageTableBuilderGeometry, TnszRootLevelThresholds) {
  /* Every threshold boundary and one step beyond, both sides. */
  EXPECT_EQ(0, root_level_for_tnsz(16));
  EXPECT_EQ(0, root_level_for_tnsz(24));
  EXPECT_EQ(1, root_level_for_tnsz(25)); /* 39-bit VA, tinyconfig anchor */
  EXPECT_EQ(1, root_level_for_tnsz(33));
  EXPECT_EQ(2, root_level_for_tnsz(34));
  EXPECT_EQ(2, root_level_for_tnsz(42));
  EXPECT_EQ(3, root_level_for_tnsz(43));
  EXPECT_EQ(3, root_level_for_tnsz(48));
  EXPECT_EQ(-1, root_level_for_tnsz(15));
  EXPECT_EQ(-1, root_level_for_tnsz(49));
  EXPECT_EQ(-1, root_level_for_tnsz(0));
}

TEST(PageTableBuilderGeometry, VaBitsToTnsz) {
  EXPECT_EQ(16U, tnsz_for_va_bits(48));
  EXPECT_EQ(25U, tnsz_for_va_bits(39));
  EXPECT_EQ(24U, tnsz_for_va_bits(40));
  /* Round trip through the threshold table. */
  EXPECT_EQ(1, root_level_for_tnsz(tnsz_for_va_bits(39)));
  EXPECT_EQ(0, root_level_for_tnsz(tnsz_for_va_bits(48)));
}

TEST(PageTableBuilderGeometry, EntryIndexPerLevel) {
  /* L1 indexes VA[38:30]: 0x40000000 is the second 1 GiB. */
  EXPECT_EQ(1, entry_index(1, 0x40000000));
  /* L2 indexes VA[29:21]: the same VA is entry 0 of its L2 table. */
  EXPECT_EQ(0, entry_index(2, 0x40000000));
  /* L3 indexes VA[20:12]. */
  EXPECT_EQ(1, entry_index(3, 0x40001234));
  /* L0 indexes VA[47:39]. */
  EXPECT_EQ(8, entry_index(0, 0x40000000000ULL)); /* 2^42 >> 39 = 2^3 */
  EXPECT_EQ(511, entry_index(0, 0xFF8000000000ULL));
}

TEST(PageTableBuilderGeometry, BlockBytesPerLevel) {
  EXPECT_EQ(0ULL, kBlockBytes[0]);       /* L0: no block form, tables only */
  EXPECT_EQ(1ULL << 30, kBlockBytes[1]); /* L1 blocks: 1 GiB */
  EXPECT_EQ(1ULL << 21, kBlockBytes[2]); /* L2 blocks: 2 MiB */
  EXPECT_EQ(1ULL << 12, kBlockBytes[3]); /* L3 pages: 4 KiB */
}

/* --- encodings: hand-computed descriptor words ----------------------------- */

TEST(PageTableBuilderEncodings, PageDescriptorWord) {
  /*
   * page @0x40001000, AttrIndx=1, AP=EL1&0 RW, inner shareable, AF, nG.
   * Hand sum: type 0x3 | PA 0x40001000 | AttrIndx 1<<2 = 0x4
   *   | SH inner 0b11<<8 = 0x300 | AF 1<<10 = 0x400 | nG 1<<11 = 0x800
   *   -> 0x40001000 | 0x3 | 0x4 | 0x300 | 0x400 | 0x800 = 0x40001F07.
   * Bit positions per hwdef.h: PTE_ATTRINDX[4:2], AP[7:6], SH[9:8],
   * PTE_AF=10, PTE_NG=11.
   */
  const uint64_t lower = lower_attrs(1, Ap::El1Rw, Shareable::Inner, true, true);
  EXPECT_EQ(0x40001F07ULL, page_desc(0x40001000, lower));
}

TEST(PageTableBuilderEncodings, PageDescriptorUpperBits) {
  /*
   * page @0x40002000, AttrIndx=0, AP=read-only-no-EL0 (0b10 -> bit 7 set),
   * non-shareable, no AF, no nG, UXN (1<<54). PA 0x40002000 = bits 30 + 13.
   * Hand sum: 0x40002000 | 0x3 | 0x80 | 0x0040000000000000
   *   -> 0x0040000040002083.
   */
  const uint64_t lower = lower_attrs(0, Ap::El1Ro, Shareable::None, false, false);
  EXPECT_EQ(0x0040000040002083ULL, page_desc(0x40002000, lower, false, true));
}

TEST(PageTableBuilderEncodings, BlockDescriptorWord) {
  /*
   * 1 GiB block @0x40000000, AttrIndx=0, AP=EL1&0 RW (00 -> no bits),
   * inner shareable, AF, no nG: 0x40000000 | 0x1 | 0x300 | 0x400
   *   -> 0x40000701.
   */
  const uint64_t lower = lower_attrs(0, Ap::El1Rw, Shareable::Inner, true, false);
  EXPECT_EQ(0x40000701ULL, block_desc(0x40000000, lower));
  /* PXN (1<<53) and contiguous (1<<52) land in the upper half. */
  EXPECT_EQ(0x0030000040000701ULL, block_desc(0x40000000, lower, true, false, true));
}

TEST(PageTableBuilderEncodings, TableDescriptorInheritanceBits) {
  /*
   * Table @0x40110000 with APTable=0b10 (read-only below), PXNTable, UXNTable:
   * 0x40110000 | 0x3 | 0b10<<61 = 0x4000000000000000
   *   | 1<<60 = 0x1000000000000000 | 1<<59 = 0x0800000000000000
   *   -> 0x5800000040110003.
   */
  EXPECT_EQ(0x5800000040110003ULL, table_desc(0x40110000, false, 0b10, true, true));
  /* Defaults: just the next-table PA and the 0b11 type. */
  EXPECT_EQ(0x40120003ULL, table_desc(0x40120000));
}

TEST(PageTableBuilderEncodings, MairAttrPlacement) {
  EXPECT_EQ(0xFFU, mair_attr(0, kMairNormalWb));
  EXPECT_EQ(0x0400U, mair_attr(1, kMairDeviceNgnre)); /* attr1 at bits[15:8] */
  EXPECT_EQ(0x44ULL << 16, mair_attr(2, kMairNormalNc));
}

/* --- mapping placement ------------------------------------------------------ */

class PageTableBuilderMap : public ::testing::Test {
 protected:
  void SetUp() override {
    image_.assign(kImageBytes, 0);
    img_.data = image_.data();
    img_.size = image_.size();
    img_.base = kBase;
    ASSERT_EQ(PtbStatus::kOk, table_clear(img_, kRoot));
    ASSERT_EQ(PtbStatus::kOk, arena_init(arena_, img_, kRoot + kTableBytes));
  }

  static constexpr uint64_t kBase = 0x40000000;
  static constexpr uint64_t kImageBytes = 262144; /* 256 KiB: root + arena + slack */
  static constexpr uint64_t kRoot = 0x40010000;

  std::vector<uint8_t> image_;
  PhysImage img_{};
  TableArena arena_{};
};

TEST_F(PageTableBuilderMap, TwoMiBBlockIdentityAtL2) {
  const uint64_t lower = lower_attrs(0, Ap::El1Rw, Shareable::Inner, true, false);
  ASSERT_EQ(PtbStatus::kOk,
            map_range(img_, arena_, kRoot, 1, 0x40000000, 0x40000000, 1ULL << 21, 2, lower));
  /* A 2 MiB block lives in an L2 table: L1[1] is the table pointer. */
  const uint64_t l2_pa = kRoot + kTableBytes;
  EXPECT_EQ(l2_pa | 0x3ULL, desc_read(img_, kRoot, 1));
  EXPECT_EQ(0x40000701ULL, desc_read(img_, l2_pa, 0)); /* the block word */
  /* Neighbour entries stay invalid: the walk must fault on unmapped VAs. */
  EXPECT_EQ(0U, desc_read(img_, kRoot, 0));
  EXPECT_EQ(0U, desc_read(img_, kRoot, 2));
  EXPECT_EQ(l2_pa + kTableBytes, arena_.cursor);
}

TEST_F(PageTableBuilderMap, ThreeLevelChainForTwoPages) {
  const uint64_t lower = lower_attrs(0, Ap::El01Rw, Shareable::Inner, true, true);
  ASSERT_EQ(PtbStatus::kOk,
            map_range(img_, arena_, kRoot, 1, 0x80000000, 0x40004000, 2 * kPageSize, 3, lower));
  /*
   * Root L1[2] -> L2 (first arena slot) -> L3 (second arena slot);
   * two consecutive page descriptors at L3[0], L3[1].
   * Hand sum for each page word: PA | 0x3 | AttrIndx 0 | AP 0b01<<6 = 0x40
   *   | SH inner 0x300 | AF 0x400 | nG 0x800 -> PA | 0xF43.
   */
  const uint64_t l2_pa = kRoot + kTableBytes;
  const uint64_t l3_pa = l2_pa + kTableBytes;
  EXPECT_EQ(l2_pa | 0x3ULL, desc_read(img_, kRoot, 2));
  EXPECT_EQ(l3_pa | 0x3ULL, desc_read(img_, l2_pa, 0));
  EXPECT_EQ(0x40004F43ULL, desc_read(img_, l3_pa, 0));
  EXPECT_EQ(0x40005F43ULL, desc_read(img_, l3_pa, 1));
  EXPECT_EQ(0U, desc_read(img_, l3_pa, 2)); /* only two pages were mapped */
  EXPECT_EQ(l3_pa + kTableBytes, arena_.cursor);
}

TEST_F(PageTableBuilderMap, MixedBlocksAndPagesCoexist) {
  const uint64_t lower = lower_attrs(0, Ap::El1Rw, Shareable::Inner, true, false);
  ASSERT_EQ(PtbStatus::kOk,
            map_range(img_, arena_, kRoot, 1, 0x40000000, 0x40000000, 1ULL << 21, 2, lower));
  /* A page mapping in a different L1 slot must not disturb the block. */
  ASSERT_EQ(PtbStatus::kOk,
            map_range(img_, arena_, kRoot, 1, 0x80000000, 0x40004000, kPageSize, 3, lower));
  const uint64_t l2_for_block = kRoot + kTableBytes;        /* 0x40011000 */
  const uint64_t l2_for_pages = l2_for_block + kTableBytes; /* 0x40012000 */
  const uint64_t l3_for_pages = l2_for_pages + kTableBytes; /* 0x40013000 */
  EXPECT_EQ(l2_for_block | 0x3ULL, desc_read(img_, kRoot, 1));
  EXPECT_EQ(0x40000701ULL, desc_read(img_, l2_for_block, 0));
  EXPECT_EQ(l2_for_pages | 0x3ULL, desc_read(img_, kRoot, 2));
  EXPECT_EQ(l3_for_pages | 0x3ULL, desc_read(img_, l2_for_pages, 0));
  EXPECT_EQ(0x40004703ULL, desc_read(img_, l3_for_pages, 0));
}

/* --- error paths ------------------------------------------------------------ */

TEST_F(PageTableBuilderMap, RejectsUnalignedLeafArguments) {
  const uint64_t lower = lower_attrs(0, Ap::El1Rw, Shareable::None, true, false);
  EXPECT_EQ(PtbStatus::kMisaligned,
            map_range(img_, arena_, kRoot, 1, 0x40000800, 0x40004000, kPageSize, 3, lower));
  EXPECT_EQ(PtbStatus::kMisaligned,
            map_range(img_, arena_, kRoot, 1, 0x40000000, 0x40004800, kPageSize, 3, lower));
  EXPECT_EQ(PtbStatus::kMisaligned,
            map_range(img_, arena_, kRoot, 1, 0x40000000, 0x40004000, kPageSize + 4, 3, lower));
}

TEST_F(PageTableBuilderMap, RejectsRemapAndBlockOverTable) {
  const uint64_t lower = lower_attrs(0, Ap::El1Rw, Shareable::None, true, false);
  ASSERT_EQ(PtbStatus::kOk,
            map_range(img_, arena_, kRoot, 1, 0x80000000, 0x40004000, kPageSize, 3, lower));
  /* The same VA already has a page: a silent second write would shadow it. */
  EXPECT_EQ(PtbStatus::kMisaligned,
            map_range(img_, arena_, kRoot, 1, 0x80000000, 0x40004000, kPageSize, 3, lower));
  /* A 2 MiB block would cover the existing L3 table chain. */
  EXPECT_EQ(PtbStatus::kMisaligned,
            map_range(img_, arena_, kRoot, 1, 0x80000000, 0x40004000, 1ULL << 21, 2, lower));
}

TEST_F(PageTableBuilderMap, RejectsLeafAboveRoot) {
  const uint64_t lower = lower_attrs(0, Ap::El1Rw, Shareable::None, true, false);
  /* Root at L1 cannot map 1 GiB blocks "at L0": L0 has no block form. */
  EXPECT_EQ(PtbStatus::kMisaligned,
            map_range(img_, arena_, kRoot, 1, 0x40000000, 0x40000000, 1ULL << 21, 0, lower));
  EXPECT_EQ(PtbStatus::kMisaligned,
            map_range(img_, arena_, kRoot, 1, 0x40000000, 0x40000000, 1ULL << 21, 4, lower));
}

TEST(PageTableBuilderErrors, ArenaExhaustionIsReported) {
  /* A 16 KiB image fits exactly one root + three arena tables. */
  std::vector<uint8_t> image(16384, 0);
  PhysImage img{image.data(), image.size(), 0x40000000};
  const uint64_t root = 0x40000000;
  ASSERT_EQ(PtbStatus::kOk, table_clear(img, root));
  TableArena arena;
  ASSERT_EQ(PtbStatus::kOk, arena_init(arena, img, root + kTableBytes));
  const uint64_t lower = lower_attrs(0, Ap::El1Rw, Shareable::None, true, false);
  /* Root L0 + L1 + L2 + L3 tables: exactly the arena's capacity. */
  ASSERT_EQ(PtbStatus::kOk,
            map_range(img, arena, root, 0, 0x00000000, 0x40000000, kPageSize, 3, lower));
  /* A second L0 slot (VA[47:39] = 8, i.e. 2^42) needs a fourth arena table. */
  EXPECT_EQ(PtbStatus::kNoRoom,
            map_range(img, arena, root, 0, 0x40000000000ULL, 0x40000000, kPageSize, 3, lower));
}

TEST(PageTableBuilderErrors, OutOfRangeAccessIsRejected) {
  std::vector<uint8_t> image(4096, 0);
  PhysImage img{image.data(), image.size(), 0x40000000};
  EXPECT_EQ(PtbStatus::kOutOfRange, table_clear(img, 0x40000000 - kTableBytes));
  EXPECT_EQ(PtbStatus::kOutOfRange, table_clear(img, 0x40001000));
  /* Descriptor accessors clamp: read gives 0, write is a no-op, no crash. */
  EXPECT_EQ(0U, desc_read(img, 0x40001000, 0));
  desc_write(img, 0x40001000, 0, 0xDEAD);
  EXPECT_EQ(0U, desc_read(img, 0x40000000, 0));
}

/* --- the image is guest-visible RAM ---------------------------------------- */

class PageTableBuilderAspace : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_EQ(OEMU_OK, oemu_aspace_init(&as_, 1)); }
  void TearDown() override {
    oemu_aspace_dispose(&as_);
    EXPECT_EQ(0U, tracker_.live_blocks()) << "aspace leaked memory";
  }

  oemu_test::TrackingAllocator tracker_;
  oemu_aspace as_{};
};

TEST_F(PageTableBuilderAspace, ImageAliasesIntoTheBusAndReadsBack) {
  std::vector<uint8_t> image(kTableBytes, 0);
  PhysImage img{image.data(), image.size(), 0x40100000};
  ASSERT_EQ(PtbStatus::kOk, table_clear(img, 0x40100000));
  desc_write(img, 0x40100000, 1, table_desc(0x40111000));

  /*
   * The alias maps the builder's own bytes into the bus -- zero copies. A
   * walker reading descriptors through memops therefore sees exactly what the
   * builder wrote, and the bus read below proves the byte order: the word
   * leaves the host buffer little-endian regardless of host endianness.
   */
  ASSERT_EQ(OEMU_OK, oemu_aspace_map_ram_alias(&as_, 0x40100000, image.data(), image.size(),
                                               OEMU_PERM_READ | OEMU_PERM_WRITE));
  uint64_t value = 0;
  ASSERT_EQ(OEMU_OK, oemu_aspace_read(&as_, 0x40100000 + 8, OEMU_MEM_DWORD, false, &value));
  EXPECT_EQ(0x40111003ULL, value);
}

} /* namespace */
