/*
 * Tests for the physical address space: region rules, RAM/device routing,
 * and the pure arithmetic that carries the correctness risk (containment at
 * the top of the address space, overlap, little-endian assembly).
 *
 * The device double below is deliberately dumb: it records every callback so
 * a test can prove the bus -- not the device -- enforced alignment, masking
 * and offset translation, and it can be told to fail so error propagation is
 * covered.
 */
#include "oemu/aspace.h"

#include <array>
#include <cstring>

#include <gtest/gtest.h>

#include "aspace/aspace_internal.h"
#include "support/tracking_allocator.h"

namespace {

constexpr uint64_t kRam = UINT64_C(0x40000000);
constexpr uint64_t kDev = UINT64_C(0x09000000); /* PL011-shaped address */

/* ---- device double ------------------------------------------------------ */

struct FakeDevice {
  oemu_status read_st = OEMU_OK; /* what the next read callback returns */
  oemu_status write_st = OEMU_OK;
  uint64_t read_value = 0; /* what the next read callback stores */
  uint64_t last_offset = 0;
  oemu_mem_size last_size = OEMU_MEM_DWORD;
  uint64_t last_write = 0;
  int reads = 0;
  int writes = 0;

  static oemu_status Read(void *ctx, uint64_t offset, oemu_mem_size size, uint64_t *value_out);
  static oemu_status Write(void *ctx, uint64_t offset, oemu_mem_size size, uint64_t value);
};

oemu_status FakeDevice::Read(void *ctx, uint64_t offset, oemu_mem_size size,
                             uint64_t *value_out) {
  auto *dev = static_cast<FakeDevice *>(ctx);
  ++dev->reads;
  dev->last_offset = offset;
  dev->last_size = size;
  if (dev->read_st != OEMU_OK) {
    return dev->read_st;
  }
  *value_out = dev->read_value;
  return OEMU_OK;
}

oemu_status FakeDevice::Write(void *ctx, uint64_t offset, oemu_mem_size size, uint64_t value) {
  auto *dev = static_cast<FakeDevice *>(ctx);
  ++dev->writes;
  dev->last_offset = offset;
  dev->last_size = size;
  dev->last_write = value;
  return dev->write_st;
}

oemu_device_ops MakeOps(FakeDevice *dev) {
  oemu_device_ops ops{};
  ops.ctx = dev;
  ops.read = &FakeDevice::Read;
  ops.write = &FakeDevice::Write;
  return ops;
}

/* ---- fixtures ----------------------------------------------------------- */

class AspaceTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_EQ(oemu_aspace_init(&as_, 8U), OEMU_OK); }
  void TearDown() override {
    oemu_aspace_dispose(&as_);
    EXPECT_FALSE(tracker_.has_leaks());
  }

  /* Maps owned RAM with `perms` and asserts success. */
  void MapRam(uint64_t pa, uint64_t size, uint32_t perms = OEMU_PERM_ALL) {
    ASSERT_EQ(oemu_aspace_map_ram(&as_, pa, size, perms, nullptr), OEMU_OK);
  }

  oemu_aspace as_{};
  oemu_test::TrackingAllocator tracker_;
};

/* ---- pure arithmetic ---------------------------------------------------- */

TEST(AspaceInternal, CheckMapArgsCoversWholeContract) {
  EXPECT_EQ(oemu_aspace_internal_check_map_args(kRam, 0U, OEMU_PERM_ALL), OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(oemu_aspace_internal_check_map_args(kRam, 0x1000U, 0U), OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(oemu_aspace_internal_check_map_args(kRam, 0x1000U, OEMU_PERM_ALL), OEMU_OK);
  EXPECT_EQ(oemu_aspace_internal_check_map_args(UINT64_MAX - 3U, 8U, OEMU_PERM_ALL),
            OEMU_ERR_OVERFLOW);
  EXPECT_EQ(oemu_aspace_internal_check_map_args(UINT64_MAX - 3U, 4U, OEMU_PERM_ALL),
            OEMU_OK); /* pa+size landing exactly on 2^64 does not wrap */
}

TEST(AspaceInternal, RangesOverlap) {
  EXPECT_FALSE(
      oemu_aspace_internal_ranges_overlap(0U, 10U, 10U, 10U)); /* a ends where b starts */
  EXPECT_FALSE(oemu_aspace_internal_ranges_overlap(10U, 10U, 0U, 10U)); /* mirrored */
  EXPECT_FALSE(oemu_aspace_internal_ranges_overlap(0U, 10U, 20U, 10U)); /* disjoint */
  EXPECT_TRUE(oemu_aspace_internal_ranges_overlap(0U, 10U, 9U, 10U)); /* one byte of contact */
  EXPECT_TRUE(oemu_aspace_internal_ranges_overlap(0U, 10U, 0U, 10U)); /* identical */
  EXPECT_TRUE(oemu_aspace_internal_ranges_overlap(0U, 100U, 40U, 10U)); /* a contains b */
  EXPECT_TRUE(oemu_aspace_internal_ranges_overlap(40U, 10U, 0U, 100U)); /* b contains a */
}

TEST(AspaceInternal, DisassembleAndAssembleAreLittleEndian) {
  const std::array<uint8_t, 8> bytes{1, 2, 3, 4, 5, 6, 7, 8};
  EXPECT_EQ(oemu_aspace_internal_disassemble(bytes.data(), 1U), 1U);
  EXPECT_EQ(oemu_aspace_internal_disassemble(bytes.data(), 2U), 0x0201U);
  EXPECT_EQ(oemu_aspace_internal_disassemble(bytes.data(), 4U), 0x04030201U);
  EXPECT_EQ(oemu_aspace_internal_disassemble(bytes.data(), 8U), UINT64_C(0x0807060504030201));

  std::array<uint8_t, 8> out{};
  oemu_aspace_internal_assemble(out.data(), 4U, UINT64_C(0xDEADBEEF12345678));
  EXPECT_EQ(out[0], 0x78U);
  EXPECT_EQ(out[1], 0x56U);
  EXPECT_EQ(out[2], 0x34U);
  EXPECT_EQ(out[3], 0x12U);
  EXPECT_EQ(out[4], 0U); /* assemble writes only the transferred bytes */
}

TEST(AspaceInternal, FindRespectsContainmentPermissionsAndEdges) {
  /* Built field by field: designated initialisers are a C extension, and
   * this is a C++ translation unit. */
  oemu_aspace_region regions[2]{};
  regions[0].pa = kRam;
  regions[0].size = 0x1000U;
  regions[0].perms = OEMU_PERM_READ | OEMU_PERM_WRITE;
  regions[0].kind = OEMU_REGION_RAM;
  regions[1].pa = kDev;
  regions[1].size = 0x1000U;
  regions[1].perms = OEMU_PERM_READ | OEMU_PERM_WRITE;
  regions[1].kind = OEMU_REGION_DEVICE;
  oemu_aspace as{};
  as.regions = regions;
  as.region_count = 2U;

  EXPECT_EQ(oemu_aspace_internal_find(&as, kRam, 0x1000U, OEMU_PERM_READ), &regions[0]);
  EXPECT_EQ(oemu_aspace_internal_find(&as, kRam + 0xFFCU, 4U, OEMU_PERM_WRITE), &regions[0]);
  EXPECT_EQ(oemu_aspace_internal_find(&as, kRam, 0x1001U, OEMU_PERM_READ),
            nullptr); /* sticks out by one byte */
  EXPECT_EQ(oemu_aspace_internal_find(&as, kRam, 0U, OEMU_PERM_READ), nullptr); /* zero size */
  EXPECT_EQ(oemu_aspace_internal_find(&as, kRam, 8U, OEMU_PERM_EXEC),
            nullptr); /* under-permissioned */
  EXPECT_EQ(oemu_aspace_internal_find(&as, kRam - 4U, 8U, OEMU_PERM_READ),
            nullptr); /* crosses the low edge */
  EXPECT_EQ(oemu_aspace_internal_find(&as, kDev, 4U, OEMU_PERM_READ | OEMU_PERM_WRITE),
            &regions[1]);
  EXPECT_EQ(oemu_aspace_internal_find(nullptr, kRam, 4U, OEMU_PERM_READ), nullptr);
}

TEST(AspaceInternal, FindResolvesTopOfAddressSpaceWithoutWrapping) {
  uint8_t filler[16]{};
  oemu_aspace_region region{};
  region.pa = UINT64_MAX - 15U; /* the region ends exactly at UINT64_MAX */
  region.size = 16U;
  region.perms = OEMU_PERM_ALL;
  region.kind = OEMU_REGION_RAM;
  region.host = filler;
  oemu_aspace as{};
  as.regions = &region;
  as.region_count = 1U;
  EXPECT_EQ(oemu_aspace_internal_find(&as, UINT64_MAX - 3U, 4U, OEMU_PERM_ALL), &region);
  EXPECT_EQ(oemu_aspace_internal_find(&as, UINT64_MAX - 3U, 8U, OEMU_PERM_ALL),
            nullptr); /* would run past UINT64_MAX */
}

/* ---- lifecycle ---------------------------------------------------------- */

TEST(AspaceLifecycle, InitRejectsNullAndZeroCapacity) {
  oemu_aspace as{};
  EXPECT_EQ(oemu_aspace_init(nullptr, 4U), OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(oemu_aspace_init(&as, 0U), OEMU_ERR_INVALID_ARG);
}

TEST(AspaceLifecycle, InitTableOomLeavesStructUnusableAndSafe) {
  oemu_test::FailingAllocator failing(1U);
  oemu_aspace as{};
  EXPECT_EQ(oemu_aspace_init(&as, 4U), OEMU_ERR_NO_MEMORY);
  EXPECT_TRUE(failing.did_fail());
  EXPECT_EQ(as.regions, nullptr);
  oemu_aspace_dispose(&as); /* dispose after a failed init must be safe */
}

TEST(AspaceLifecycle, DisposeIsIdempotentAndNullSafe) {
  oemu_aspace as{};
  ASSERT_EQ(oemu_aspace_init(&as, 4U), OEMU_OK);
  ASSERT_EQ(oemu_aspace_map_ram(&as, kRam, 0x1000U, OEMU_PERM_ALL, nullptr), OEMU_OK);
  oemu_aspace_dispose(&as);
  oemu_aspace_dispose(&as);
  EXPECT_EQ(as.region_count, 0U);
  oemu_aspace_dispose(nullptr);
}

TEST(AspaceLifecycle, FullLifecycleLeavesNothingLive) {
  oemu_test::TrackingAllocator tracker;
  oemu_aspace as{};
  ASSERT_EQ(oemu_aspace_init(&as, 4U), OEMU_OK);
  ASSERT_EQ(oemu_aspace_map_ram(&as, kRam, 0x1000U, OEMU_PERM_ALL, nullptr), OEMU_OK);
  ASSERT_EQ(oemu_aspace_map_ram(&as, kRam + 0x1000U, 0x1000U, OEMU_PERM_ALL, nullptr), OEMU_OK);
  oemu_aspace_dispose(&as);
  EXPECT_EQ(tracker.alloc_count(), 3U); /* table + two RAM blocks */
  EXPECT_FALSE(tracker.has_leaks());
}

/* ---- mapping rules -------------------------------------------------------- */

TEST_F(AspaceTest, MapRejectsBadArgsOverlapCapacityAndWrap) {
  EXPECT_EQ(oemu_aspace_map_ram(nullptr, kRam, 0x1000U, OEMU_PERM_ALL, nullptr),
            OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(oemu_aspace_map_ram(&as_, kRam, 0U, OEMU_PERM_ALL, nullptr), OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(oemu_aspace_map_ram(&as_, kRam, 0x1000U, 0U, nullptr), OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(oemu_aspace_map_ram(&as_, UINT64_MAX - 3U, 8U, OEMU_PERM_ALL, nullptr),
            OEMU_ERR_OVERFLOW);
  MapRam(kRam, 0x1000U);
  EXPECT_EQ(oemu_aspace_map_ram(&as_, kRam, 0x10U, OEMU_PERM_ALL, nullptr), OEMU_ERR_RANGE);
  EXPECT_EQ(oemu_aspace_map_ram(&as_, kRam + 0x800U, 0x2000U, OEMU_PERM_ALL, nullptr),
            OEMU_ERR_RANGE);
  EXPECT_EQ(oemu_aspace_map_ram(&as_, kRam + 0x1000U, 0x1000U, OEMU_PERM_ALL, nullptr),
            OEMU_OK); /* adjacent is not overlapping */
  for (unsigned i = 0U; i < 6U; ++i) {
    MapRam(kRam + UINT64_C(0x100000) * (i + 1U), 0x1000U); /* i == 0 taken above */
  }
  EXPECT_EQ(as_.region_count, 8U);
  EXPECT_EQ(oemu_aspace_map_ram(&as_, UINT64_C(0x80000000), 0x1000U, OEMU_PERM_ALL, nullptr),
            OEMU_ERR_RANGE); /* table full */
}

TEST_F(AspaceTest, MapRamOomChangesNothing) {
  /* Fresh minimal table so the failing call lines up exactly (call 1 is the
   * table, call 2 the RAM block), and dispose inside the failing scope --
   * the aspace caches the allocator it was initialised with. */
  oemu_aspace_dispose(&as_);
  {
    oemu_test::FailingAllocator failing(2U);
    ASSERT_EQ(oemu_aspace_init(&as_, 1U), OEMU_OK);
    EXPECT_EQ(oemu_aspace_map_ram(&as_, kRam, 0x1000U, OEMU_PERM_ALL, nullptr),
              OEMU_ERR_NO_MEMORY);
    EXPECT_EQ(as_.region_count, 0U); /* a failed map leaves no half-built region */
    oemu_aspace_dispose(&as_);
  }
  ASSERT_EQ(oemu_aspace_init(&as_, 8U), OEMU_OK); /* restore for TearDown */
  ASSERT_EQ(oemu_aspace_map_ram(&as_, kRam, 0x1000U, OEMU_PERM_ALL, nullptr),
            OEMU_OK); /* the bus still works afterwards */
}

TEST_F(AspaceTest, MapRamHandsBackOwnedBlockAndZeroFill) {
  void *host = nullptr;
  ASSERT_EQ(oemu_aspace_map_ram(&as_, kRam, 0x100U, OEMU_PERM_ALL, &host), OEMU_OK);
  ASSERT_NE(host, nullptr);
  const auto *bytes = static_cast<const uint8_t *>(host);
  EXPECT_EQ(bytes[0], 0U);
  EXPECT_EQ(bytes[0xFF], 0U);
  std::memset(host, 0xAB, 0x100U); /* the loader's back door really is the RAM */
  uint64_t value = 0;
  ASSERT_EQ(oemu_aspace_read(&as_, kRam, OEMU_MEM_WORD, false, &value), OEMU_OK);
  EXPECT_EQ(value, 0xABABABABU);
}

TEST(AspaceMapAlias, AliasIsBorrowedNotFreed) {
  oemu_test::TrackingAllocator tracker;
  std::array<uint8_t, 0x100> buffer{};
  oemu_aspace as{};
  ASSERT_EQ(oemu_aspace_init(&as, 2U), OEMU_OK);
  ASSERT_EQ(oemu_aspace_map_ram_alias(&as, kRam, buffer.data(), buffer.size(), OEMU_PERM_ALL),
            OEMU_OK);
  ASSERT_EQ(oemu_aspace_write(&as, kRam, OEMU_MEM_WORD, 0x11223344U), OEMU_OK);
  EXPECT_EQ(buffer[0], 0x44U); /* writes land in the caller's buffer */
  buffer[4] = 0x55;
  uint64_t value = 0;
  ASSERT_EQ(oemu_aspace_read(&as, kRam + 4U, OEMU_MEM_BYTE, false, &value), OEMU_OK);
  EXPECT_EQ(value, 0x55U); /* and reads see the caller's writes */
  oemu_aspace_dispose(&as);
  EXPECT_EQ(tracker.alloc_count(), 1U); /* only the table was allocated */
  EXPECT_FALSE(tracker.has_leaks());
}

TEST_F(AspaceTest, MapAliasRejectsNullHostAndBadRanges) {
  std::array<uint8_t, 16> buffer{};
  EXPECT_EQ(oemu_aspace_map_ram_alias(&as_, kRam, nullptr, 16U, OEMU_PERM_ALL),
            OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(oemu_aspace_map_ram_alias(&as_, kRam, buffer.data(), 0U, OEMU_PERM_ALL),
            OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(oemu_aspace_map_ram_alias(&as_, kRam, buffer.data(), 16U, 0U),
            OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(oemu_aspace_map_ram_alias(&as_, UINT64_MAX, buffer.data(), 16U, OEMU_PERM_ALL),
            OEMU_ERR_OVERFLOW);
}

/* ---- attach rules --------------------------------------------------------- */

TEST_F(AspaceTest, AttachDeviceValidatesOpsSizeAndAlignment) {
  FakeDevice dev;
  oemu_device_ops ops = MakeOps(&dev);
  EXPECT_EQ(oemu_aspace_attach_device(&as_, kDev, 0x1000U, nullptr), OEMU_ERR_INVALID_ARG);
  oemu_device_ops no_read = ops;
  no_read.read = nullptr;
  EXPECT_EQ(oemu_aspace_attach_device(&as_, kDev, 0x1000U, &no_read), OEMU_ERR_INVALID_ARG);
  oemu_device_ops no_write = ops;
  no_write.write = nullptr;
  EXPECT_EQ(oemu_aspace_attach_device(&as_, kDev, 0x1000U, &no_write), OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(oemu_aspace_attach_device(&as_, kDev, 0U, &ops), OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(oemu_aspace_attach_device(&as_, kDev, 0x1234U, &ops),
            OEMU_ERR_INVALID_ARG); /* not a power of two */
  EXPECT_EQ(oemu_aspace_attach_device(&as_, kDev + 4U, 0x1000U, &ops),
            OEMU_ERR_INVALID_ARG); /* not aligned to its own size */
  MapRam(kDev, 0x1000U);
  EXPECT_EQ(oemu_aspace_attach_device(&as_, kDev, 0x1000U, &ops), OEMU_ERR_RANGE); /* overlap */
  EXPECT_EQ(as_.region_count, 1U);
  ASSERT_EQ(oemu_aspace_attach_device(&as_, kDev + 0x1000U, 0x1000U, &ops),
            OEMU_OK);              /* the next free, self-aligned window */
  EXPECT_EQ(as_.region_count, 2U); /* a device attach allocated nothing */
  EXPECT_EQ(tracker_.alloc_count(), 2U);
}

/* ---- bus access: RAM ------------------------------------------------------- */

TEST_F(AspaceTest, RamAccessesAreLittleEndianAtEveryWidth) {
  MapRam(kRam, 0x1000U);
  ASSERT_EQ(oemu_aspace_write(&as_, kRam, OEMU_MEM_DWORD, UINT64_C(0x89ABCDEF01234567)),
            OEMU_OK);
  uint8_t raw[8]{};
  void *host = as_.regions[0].host;
  std::memcpy(raw, host, 8U);
  EXPECT_EQ(raw[0], 0x67U); /* the host's byte order must not leak */
  EXPECT_EQ(raw[7], 0x89U);
  uint64_t value = 0;
  ASSERT_EQ(oemu_aspace_read(&as_, kRam, OEMU_MEM_BYTE, false, &value), OEMU_OK);
  EXPECT_EQ(value, 0x67U);
  ASSERT_EQ(oemu_aspace_read(&as_, kRam, OEMU_MEM_HALF, false, &value), OEMU_OK);
  EXPECT_EQ(value, 0x4567U);
  ASSERT_EQ(oemu_aspace_read(&as_, kRam, OEMU_MEM_WORD, false, &value), OEMU_OK);
  EXPECT_EQ(value, 0x01234567U);
  ASSERT_EQ(oemu_aspace_read(&as_, kRam, OEMU_MEM_DWORD, false, &value), OEMU_OK);
  EXPECT_EQ(value, UINT64_C(0x89ABCDEF01234567));
}

TEST_F(AspaceTest, SignExtensionMatchesArmLoadSemantics) {
  MapRam(kRam, 0x1000U);
  ASSERT_EQ(oemu_aspace_write(&as_, kRam, OEMU_MEM_BYTE, 0x80U), OEMU_OK);
  ASSERT_EQ(oemu_aspace_write(&as_, kRam + 8U, OEMU_MEM_HALF, 0x8000U), OEMU_OK);
  ASSERT_EQ(oemu_aspace_write(&as_, kRam + 16U, OEMU_MEM_WORD, 0x80000000U), OEMU_OK);
  uint64_t value = 0;
  ASSERT_EQ(oemu_aspace_read(&as_, kRam, OEMU_MEM_BYTE, false, &value), OEMU_OK);
  EXPECT_EQ(value, 0x80U);
  ASSERT_EQ(oemu_aspace_read(&as_, kRam, OEMU_MEM_BYTE, true, &value), OEMU_OK);
  EXPECT_EQ(value, UINT64_C(0xFFFFFFFFFFFFFF80));
  ASSERT_EQ(oemu_aspace_read(&as_, kRam + 8U, OEMU_MEM_HALF, true, &value), OEMU_OK);
  EXPECT_EQ(value, UINT64_C(0xFFFFFFFFFFFF8000));
  ASSERT_EQ(oemu_aspace_read(&as_, kRam + 16U, OEMU_MEM_WORD, true, &value), OEMU_OK);
  EXPECT_EQ(value, UINT64_C(0xFFFFFFFF80000000));
  /* A dword is already full width: the sign flag cannot stretch it. */
  ASSERT_EQ(oemu_aspace_read(&as_, kRam, OEMU_MEM_DWORD, true, &value), OEMU_OK);
  EXPECT_EQ(value, 0x80U);
}

TEST_F(AspaceTest, AccessesObeyPermissionsAndBoundaries) {
  MapRam(kRam, 0x1000U, OEMU_PERM_READ);
  MapRam(kRam + 0x2000U, 0x1000U, OEMU_PERM_WRITE);
  uint64_t value = 0;
  EXPECT_EQ(oemu_aspace_read(&as_, kRam, OEMU_MEM_DWORD, false, &value), OEMU_OK);
  EXPECT_EQ(oemu_aspace_write(&as_, kRam, OEMU_MEM_DWORD, 1U), OEMU_ERR_FAULT);
  EXPECT_EQ(oemu_aspace_read(&as_, kRam + 0x2000U, OEMU_MEM_DWORD, false, &value),
            OEMU_ERR_FAULT);
  EXPECT_EQ(oemu_aspace_write(&as_, kRam + 0x2000U, OEMU_MEM_DWORD, 1U), OEMU_OK);
  /* Straddling the gap between two regions must fault, not split. */
  MapRam(kRam + 0x1000U, 0x1000U);
  EXPECT_EQ(oemu_aspace_read(&as_, kRam + 0xFFCU, OEMU_MEM_DWORD, false, &value),
            OEMU_ERR_FAULT);
  EXPECT_EQ(oemu_aspace_validate(&as_, kRam + 0xFFCU, 8U, OEMU_PERM_READ), OEMU_ERR_FAULT);
  EXPECT_EQ(oemu_aspace_read(&as_, kRam + 0x1000U, OEMU_MEM_DWORD, false, &value), OEMU_OK);
  EXPECT_EQ(oemu_aspace_read(&as_, UINT64_C(0xDEAD0000), OEMU_MEM_BYTE, false, &value),
            OEMU_ERR_FAULT); /* unmapped */
}

TEST_F(AspaceTest, AccessRejectsNullBusBadSizeAndNullValueOut) {
  uint64_t value = 0;
  EXPECT_EQ(oemu_aspace_read(nullptr, kRam, OEMU_MEM_WORD, false, &value),
            OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(oemu_aspace_read(&as_, kRam, OEMU_MEM_WORD, false, nullptr), OEMU_ERR_INVALID_ARG);
  /* The size selector is an unguarded uint8_t off the instruction word, so a
   * non-encoding (4..7) must still be refused. Feeding it through an int keeps
   * the literal out of -Wconversion, which would call the cast itself UB. */
  const int bad_sizes[] = {4, 7};
  EXPECT_EQ(
      oemu_aspace_read(&as_, kRam, static_cast<oemu_mem_size>(bad_sizes[0]), false, &value),
      OEMU_ERR_INVALID_ARG); /* not one of the four encodings */
  EXPECT_EQ(oemu_aspace_write(&as_, kRam, static_cast<oemu_mem_size>(bad_sizes[1]), 0U),
            OEMU_ERR_INVALID_ARG);
  /* validate takes a plain byte count instead, so only zero is ill-formed. */
  EXPECT_EQ(oemu_aspace_validate(&as_, kRam, 0U, OEMU_PERM_READ), OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(oemu_aspace_validate(nullptr, kRam, 4U, OEMU_PERM_READ), OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(oemu_aspace_write(nullptr, kRam, OEMU_MEM_WORD, 0U), OEMU_ERR_INVALID_ARG);
}

TEST_F(AspaceTest, AccessTopOfAddressSpace) {
  /* One byte at the very top: the containment arithmetic's hardest case. */
  ASSERT_EQ(oemu_aspace_map_ram(&as_, UINT64_MAX, 1U, OEMU_PERM_ALL, nullptr), OEMU_OK);
  ASSERT_EQ(oemu_aspace_write(&as_, UINT64_MAX, OEMU_MEM_BYTE, 0xA5U), OEMU_OK);
  uint64_t value = 0;
  ASSERT_EQ(oemu_aspace_read(&as_, UINT64_MAX, OEMU_MEM_BYTE, false, &value), OEMU_OK);
  EXPECT_EQ(value, 0xA5U);
  EXPECT_EQ(oemu_aspace_read(&as_, UINT64_MAX - 1U, OEMU_MEM_HALF, false, &value),
            OEMU_ERR_FAULT);
}

/* ---- bus access: devices ---------------------------------------------------- */

class DeviceAccessTest : public AspaceTest {
 protected:
  void SetUpRam() {
    MapRam(kRam, 0x1000U);
    ops_ = MakeOps(&dev_);
    ASSERT_EQ(oemu_aspace_attach_device(&as_, kDev, 0x1000U, &ops_), OEMU_OK);
  }

  FakeDevice dev_;
  oemu_device_ops ops_{};
};

TEST_F(DeviceAccessTest, ReadDispatchesWithDeviceRelativeOffset) {
  SetUpRam();
  dev_.read_value = UINT64_C(0xFFFFFFFF12345678);
  uint64_t value = 0;
  ASSERT_EQ(oemu_aspace_read(&as_, kDev + 0x40U, OEMU_MEM_WORD, false, &value), OEMU_OK);
  EXPECT_EQ(value, 0x12345678U); /* the bus masked a device that leaked high bits */
  EXPECT_EQ(dev_.reads, 1);
  EXPECT_EQ(dev_.last_offset, 0x40U); /* the device never learns its own base */
  EXPECT_EQ(dev_.last_size, OEMU_MEM_WORD);
  ASSERT_EQ(oemu_aspace_read(&as_, kDev, OEMU_MEM_DWORD, false, &value), OEMU_OK);
  EXPECT_EQ(value, UINT64_C(0xFFFFFFFF12345678));
}

TEST_F(DeviceAccessTest, ReadSignExtensionAppliesToDeviceValuesToo) {
  SetUpRam();
  dev_.read_value = UINT64_MAX;
  uint64_t value = 0;
  ASSERT_EQ(oemu_aspace_read(&as_, kDev, OEMU_MEM_BYTE, false, &value), OEMU_OK);
  EXPECT_EQ(value, 0xFFU);
  ASSERT_EQ(oemu_aspace_read(&as_, kDev, OEMU_MEM_BYTE, true, &value), OEMU_OK);
  EXPECT_EQ(value, UINT64_MAX);
  ASSERT_EQ(oemu_aspace_read(&as_, kDev, OEMU_MEM_HALF, true, &value), OEMU_OK);
  EXPECT_EQ(value, UINT64_C(0xFFFFFFFFFFFFFFFF));
}

TEST_F(DeviceAccessTest, WriteMasksToTransferWidth) {
  SetUpRam();
  ASSERT_EQ(oemu_aspace_write(&as_, kDev + 8U, OEMU_MEM_BYTE, UINT64_C(0xDEADBEEF123456FF)),
            OEMU_OK);
  EXPECT_EQ(dev_.writes, 1);
  EXPECT_EQ(dev_.last_offset, 8U);
  EXPECT_EQ(dev_.last_size, OEMU_MEM_BYTE);
  EXPECT_EQ(dev_.last_write, 0xFFU); /* only the byte that was transferred */
}

TEST_F(DeviceAccessTest, MisalignedDeviceAccessFaultsWithoutCallingBack) {
  SetUpRam();
  uint64_t value = 0;
  EXPECT_EQ(oemu_aspace_read(&as_, kDev + 1U, OEMU_MEM_WORD, false, &value), OEMU_ERR_FAULT);
  EXPECT_EQ(oemu_aspace_write(&as_, kDev + 3U, OEMU_MEM_HALF, 1U), OEMU_ERR_FAULT);
  EXPECT_EQ(oemu_aspace_write(&as_, kDev + 3U, OEMU_MEM_BYTE, 1U), OEMU_OK); /* byte: any */
  EXPECT_EQ(oemu_aspace_read(&as_, kDev + 6U, OEMU_MEM_HALF, false, &value), OEMU_OK);
  /* RAM keeps the architecture's unaligned allowance: only devices fault. */
  EXPECT_EQ(oemu_aspace_read(&as_, kRam + 1U, OEMU_MEM_WORD, false, &value), OEMU_OK);
  EXPECT_EQ(dev_.reads, 1);
  EXPECT_EQ(dev_.writes, 1);
}

TEST_F(DeviceAccessTest, CallbackErrorsReachTheCallerVerbatim) {
  SetUpRam();
  dev_.read_st = OEMU_ERR_FAULT;
  uint64_t value = 0;
  EXPECT_EQ(oemu_aspace_read(&as_, kDev, OEMU_MEM_WORD, false, &value), OEMU_ERR_FAULT);
  dev_.read_st = OEMU_OK;
  dev_.write_st = OEMU_ERR_TIMEOUT; /* distinct from FAULT: proves no rewriting */
  EXPECT_EQ(oemu_aspace_write(&as_, kDev, OEMU_MEM_WORD, 1U), OEMU_ERR_TIMEOUT);
}

TEST_F(DeviceAccessTest, ValidateCoversWholeWindowWithoutAskingTheDevice) {
  SetUpRam();
  /* Any offset inside the window validates; register-level truth is the
   * device's call at the access itself. */
  EXPECT_EQ(oemu_aspace_validate(&as_, kDev + 0xF00U, 4U, OEMU_PERM_READ | OEMU_PERM_WRITE),
            OEMU_OK);
  EXPECT_EQ(oemu_aspace_validate(&as_, kDev - 8U, 8U, OEMU_PERM_READ), OEMU_ERR_FAULT);
  EXPECT_EQ(oemu_aspace_validate(&as_, kDev, 4U, OEMU_PERM_EXEC),
            OEMU_ERR_FAULT); /* devices are never executable */
  EXPECT_EQ(dev_.reads, 0);
}

/* ---- fetch ------------------------------------------------------------------- */

TEST_F(AspaceTest, FetchNeedsExecAndRamAndFourByteAlignment) {
  MapRam(kRam, 0x1000U); /* RWX here: the word arrives through the bus */
  ASSERT_EQ(oemu_aspace_write(&as_, kRam + 4U, OEMU_MEM_WORD, 0xD65F03C0U), OEMU_OK);
  uint32_t word = 0;
  ASSERT_EQ(oemu_aspace_fetch32(&as_, kRam + 4U, &word), OEMU_OK);
  EXPECT_EQ(word, 0xD65F03C0U);
  EXPECT_EQ(oemu_aspace_fetch32(&as_, kRam + 6U, &word), OEMU_ERR_INVALID_ARG); /* misaligned */
  EXPECT_EQ(oemu_aspace_fetch32(nullptr, kRam, &word), OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(oemu_aspace_fetch32(&as_, kRam, nullptr), OEMU_ERR_INVALID_ARG);
  /* A data-only region cannot serve code. */
  MapRam(kRam + 0x2000U, 0x1000U, OEMU_PERM_READ | OEMU_PERM_WRITE);
  EXPECT_EQ(oemu_aspace_fetch32(&as_, kRam + 0x2000U, &word), OEMU_ERR_FAULT);
  /* Nor can a device. */
  FakeDevice dev;
  oemu_device_ops ops = MakeOps(&dev);
  ASSERT_EQ(oemu_aspace_attach_device(&as_, kDev, 0x1000U, &ops), OEMU_OK);
  EXPECT_EQ(oemu_aspace_fetch32(&as_, kDev, &word), OEMU_ERR_FAULT);
}

TEST_F(AspaceTest, FetchRefusesToReadPastRegionEnd) {
  MapRam(kRam, 0x1004U, OEMU_PERM_ALL);
  uint32_t word = 0;
  EXPECT_EQ(oemu_aspace_fetch32(&as_, kRam, &word), OEMU_OK);
  EXPECT_EQ(oemu_aspace_fetch32(&as_, kRam + 0x1004U, &word),
            OEMU_ERR_FAULT); /* exactly off the end */
}

}  // namespace
