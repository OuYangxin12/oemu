// Tests for the boot-time virt device-tree generator (M5). These read the
// finished blob back through the internal FDT reader -- a writer proved only by
// writing proves nothing -- and pin the exact shape the guest driver binds to:
// /chosen bootargs + initrd cells, the A15-GIC, the arch timer PPIs, the PSCI
// node, and the PL011 console. White-box: it links the internal reader.

#include "oemu/allocator.h"
#include "oemu/fdt.h"
#include "oemu/virt_dtb.h"

#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "fdt/fdt_internal.h"

namespace {

constexpr uint64_t kRamBase = 0x40000000ULL;
constexpr uint64_t kRamSize = 0x10000000ULL; /* 256 MiB */

// A short-of-space property never reaches a real find, so these bytes only ever
// show up when a find failed; returning them keeps a caller indexing a value
// from dereferencing a null pointer (the EXPECT that failed already marks red).
const unsigned char kZero[32] = {0};

// Big-endian read-back helpers: the FDT wire format is big-endian throughout.
uint32_t be32(const unsigned char *p) {
  return (static_cast<uint32_t>(p[0]) << 24U) | (static_cast<uint32_t>(p[1]) << 16U) |
         (static_cast<uint32_t>(p[2]) << 8U) | p[3];
}
uint64_t be64(const unsigned char *p) {
  return (static_cast<uint64_t>(be32(p)) << 32U) | be32(p + 4U);
}

oemu_virt_dtb_params params(uint64_t irs, uint64_t ire, const char *cmd) {
  return oemu_virt_dtb_params{kRamBase, kRamSize, irs, ire, cmd, nullptr, 0U};
}

class VirtDtb : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_EQ(OEMU_OK, oemu_fdt_init(&fdt_, 64U << 10)); }
  void TearDown() override { oemu_fdt_dispose(&fdt_); }

  oemu_status build(const oemu_virt_dtb_params &p) { return oemu_virt_dtb_build(&fdt_, &p); }

  // Read one property out of the finished tree. On a failed find an EXPECT
  // marks the test red and a zero buffer is returned so the caller's be32/be64
  // stay memory-safe.
  const unsigned char *prop(const char *path, const char *name, size_t want_len) {
    const unsigned char *out = nullptr;
    size_t len = 0U;
    const oemu_status st = oemu_fdt_internal_find(oemu_fdt_bytes(&fdt_), oemu_fdt_length(&fdt_),
                                                  path, name, &out, &len);
    EXPECT_EQ(OEMU_OK, st) << "find " << path << "/" << name;
    EXPECT_EQ(want_len, len) << "length of " << path << "/" << name;
    if (st != OEMU_OK) {
      return kZero;
    }
    return out;
  }
  std::string str(const char *path, const char *name) {
    const unsigned char *out = nullptr;
    size_t len = 0U;
    const oemu_status st = oemu_fdt_internal_find(oemu_fdt_bytes(&fdt_), oemu_fdt_length(&fdt_),
                                                  path, name, &out, &len);
    EXPECT_EQ(OEMU_OK, st) << "find " << path << "/" << name;
    if (st != OEMU_OK) {
      return std::string();
    }
    while ((len > 0U) && (out[len - 1U] == '\0')) {
      len--; /* FDT strings carry a trailing NUL we do not assert on */
    }
    return std::string(reinterpret_cast<const char *>(out), len);
  }
  bool has_empty(const char *path, const char *name) {
    const unsigned char *out = nullptr;
    size_t len = 0U;
    return oemu_fdt_internal_find(oemu_fdt_bytes(&fdt_), oemu_fdt_length(&fdt_), path, name,
                                  &out, &len) == OEMU_OK;
  }

  oemu_fdt fdt_{};
};

TEST_F(VirtDtb, RootDescribesTheMachine) {
  ASSERT_EQ(OEMU_OK, build(params(0, 0, nullptr)));
  EXPECT_EQ("linux,dummy-virt", str("/", "compatible"));
  EXPECT_EQ("oemu-virt", str("/", "model"));
  EXPECT_EQ(2U, be32(prop("/", "#address-cells", 4)));
  EXPECT_EQ(2U, be32(prop("/", "#size-cells", 4)));
}

TEST_F(VirtDtb, MemoryNodeMatchesTheBuiltRam) {
  ASSERT_EQ(OEMU_OK, build(params(0, 0, nullptr)));
  const unsigned char *reg = prop("/memory@40000000", "reg", 16);
  EXPECT_EQ(0U, be32(reg + 0));
  EXPECT_EQ(0x40000000U, be32(reg + 4));
  EXPECT_EQ(0U, be32(reg + 8));
  EXPECT_EQ(0x10000000U, be32(reg + 12));
  EXPECT_EQ("memory", str("/memory@40000000", "device_type"));
}

TEST_F(VirtDtb, ChosenCarriesBootargsAndZeroInitrdWhenNone) {
  ASSERT_EQ(OEMU_OK, build(params(0, 0, "console=ttyAMA0 panic=-1")));
  EXPECT_EQ("console=ttyAMA0 panic=-1", str("/chosen", "bootargs"));
  EXPECT_EQ(0U, be64(prop("/chosen", "linux,initrd-start", 8)));
  EXPECT_EQ(0U, be64(prop("/chosen", "linux,initrd-end", 8)));
  EXPECT_EQ("/pl011@9000000", str("/chosen", "stdout-path"));
}

TEST_F(VirtDtb, ChosenPointsAtTheInitrdCells) {
  ASSERT_EQ(OEMU_OK, build(params(0x4C000000ULL, 0x4C400000ULL, nullptr)));
  EXPECT_EQ(0x4C000000ULL, be64(prop("/chosen", "linux,initrd-start", 8)));
  EXPECT_EQ(0x4C400000ULL, be64(prop("/chosen", "linux,initrd-end", 8)));
}

TEST_F(VirtDtb, RngSeedIsEmittedVerbatimAndOmittedWhenAbsent) {
  // The guest consumes /chosen/rng-seed as opaque bytes, so the tree must carry
  // them untouched -- not byte-swapped, not truncated to a cell count.
  static const uint8_t seed[32] = {0x00U, 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U,
                                   0x08U, 0x09U, 0x0AU, 0x0BU, 0x0CU, 0x0DU, 0x0EU, 0x0FU,
                                   0x10U, 0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U, 0x17U,
                                   0x18U, 0x19U, 0x1AU, 0x1BU, 0x1CU, 0x1DU, 0x1EU, 0x1FU};
  oemu_virt_dtb_params p = params(0, 0, nullptr);
  p.rng_seed = seed;
  p.rng_seed_len = (uint32_t)sizeof(seed);
  ASSERT_EQ(OEMU_OK, build(p));
  const unsigned char *out = prop("/chosen", "rng-seed", 32U);
  for (unsigned i = 0U; i < 32U; ++i) {
    EXPECT_EQ(seed[i], out[i]) << "rng-seed byte " << i;
  }

  // Without a seed the property must not be there at all: an empty rng-seed is
  // not "no seed" to the guest, it is a zero-length one. (Covered by its own
  // case because the writer is single-use once finished.)
}

TEST_F(VirtDtb, NoRngSeedPropertyWhenNoneSupplied) {
  ASSERT_EQ(OEMU_OK, build(params(0, 0, nullptr)));
  const unsigned char *absent = nullptr;
  size_t len = 0U;
  EXPECT_NE(OEMU_OK, oemu_fdt_internal_find(oemu_fdt_bytes(&fdt_), oemu_fdt_length(&fdt_),
                                            "/chosen", "rng-seed", &absent, &len));
}

TEST_F(VirtDtb, NullCmdlineUsesASafeDefault) {
  ASSERT_EQ(OEMU_OK, build(params(0, 0, nullptr)));
  EXPECT_NE(std::string::npos, str("/chosen", "bootargs").find("earlycon=pl011,0x9000000"));
}

TEST_F(VirtDtb, PsciNodeOnTheSmcConduit) {
  ASSERT_EQ(OEMU_OK, build(params(0, 0, nullptr)));
  EXPECT_EQ("smc", str("/psci", "method"));
  EXPECT_NE(std::string::npos, str("/psci", "compatible").find("arm,psci-1.0"));
  EXPECT_EQ(0xC4000003U, be32(prop("/psci", "cpu_on", 4)));
  EXPECT_EQ(0x84000002U, be32(prop("/psci", "cpu_off", 4)));
}

TEST_F(VirtDtb, SingleA53CpuEnabledByPsci) {
  ASSERT_EQ(OEMU_OK, build(params(0, 0, nullptr)));
  EXPECT_EQ("arm,cortex-a53", str("/cpus/cpu@0", "compatible"));
  EXPECT_EQ("psci", str("/cpus/cpu@0", "enable-method"));
  EXPECT_EQ(0U, be32(prop("/cpus/cpu@0", "reg", 4)));
}

TEST_F(VirtDtb, InterruptControllerIsAnA15Gic) {
  ASSERT_EQ(OEMU_OK, build(params(0, 0, nullptr)));
  EXPECT_EQ("arm,cortex-a15-gic", str("/interrupt-controller@8000000", "compatible"));
  EXPECT_EQ(3U, be32(prop("/interrupt-controller@8000000", "#interrupt-cells", 4)));
  EXPECT_TRUE(has_empty("/interrupt-controller@8000000", "interrupt-controller"));
  const unsigned char *reg = prop("/interrupt-controller@8000000", "reg", 32);
  EXPECT_EQ(0x8000000U, be32(reg + 4));  /* distributor */
  EXPECT_EQ(0x8010000U, be32(reg + 20)); /* CPU interface */
}

TEST_F(VirtDtb, TimerAdvertisesFourPpis) {
  ASSERT_EQ(OEMU_OK, build(params(0, 0, nullptr)));
  EXPECT_NE(std::string::npos, str("/timer", "compatible").find("arm,armv8-timer"));
  const unsigned char *irq = prop("/timer", "interrupts", 48); /* 4 x 12-byte specifiers */
  EXPECT_EQ(1U, be32(irq + 0));                                /* PPI type */
  EXPECT_EQ(13U, be32(irq + 4));                               /* non-secure physical */
  EXPECT_TRUE(has_empty("/timer", "always-on"));
}

TEST_F(VirtDtb, Pl011ConsoleOnSpi33) {
  ASSERT_EQ(OEMU_OK, build(params(0, 0, nullptr)));
  /* The binding the guest actually obeys: the AMBA device is created only for
   * a node whose compatible list also names the generic "arm,primecell", and
   * only below a parent the OF scan descends into ("simple-bus"). Both come
   * from drivers/of/platform.c, and issue #28 was exactly this node being
   * invisible to the scan: no port, no console, /init with no stdio. */
  const unsigned char *compat = prop("/pl011@9000000", "compatible", 24);
  ASSERT_TRUE(compat != nullptr);
  EXPECT_EQ(0, std::memcmp(compat, "arm,pl011\0", 10));
  EXPECT_EQ(0, std::memcmp(compat + 10, "arm,primecell\0", 14));
  /* Two clocks, both names present: drivers/amba/bus.c looks the second one up
   * by the literal name "apb_pclk" and defers the probe when that fails. */
  const unsigned char *names = prop("/pl011@9000000", "clock-names", 17);
  ASSERT_TRUE(names != nullptr);
  EXPECT_EQ(0, std::memcmp(names, "uartclk\0", 8));
  EXPECT_EQ(0, std::memcmp(names + 8, "apb_pclk\0", 9));
  const unsigned char *irq = prop("/pl011@9000000", "interrupts", 12);
  EXPECT_EQ(0U, be32(irq + 0)); /* SPI type */
  EXPECT_EQ(1U, be32(irq + 4)); /* SPI offset 1 -> interrupt id 33 */
  const unsigned char *reg = prop("/pl011@9000000", "reg", 16);
  EXPECT_EQ(0x9000000U, be32(reg + 4));
  EXPECT_EQ("/pl011@9000000", str("/aliases", "serial0"));
}

TEST_F(VirtDtb, HeaderTotalSizeAgreesWithBlob) {
  ASSERT_EQ(OEMU_OK, build(params(0, 0, nullptr)));
  size_t total = 0U;
  ASSERT_EQ(OEMU_OK, oemu_fdt_internal_total_size(oemu_fdt_bytes(&fdt_), oemu_fdt_length(&fdt_),
                                                  &total));
  EXPECT_EQ(oemu_fdt_length(&fdt_), total);
}

TEST_F(VirtDtb, TooSmallCapIsARecoverableStatusNotACrash) {
  // A buffer that inits (above oemu_fdt_init's floor) but cannot hold the whole
  // tree is a recoverable status -- NO_MEMORY from a mid-tree emit -- not a
  // crash or a truncated blob.
  oemu_fdt small{};
  ASSERT_EQ(OEMU_OK, oemu_fdt_init(&small, 300U));
  const auto p = params(0, 0, nullptr);
  EXPECT_NE(OEMU_OK, oemu_virt_dtb_build(&small, &p));
  oemu_fdt_dispose(&small);
}

}  // namespace
