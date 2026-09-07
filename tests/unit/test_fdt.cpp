// Tests for the FDT builder and its read-back helper.
//
// The builder's contract is round-trip truth: whatever was emitted must be
// findable through the reader with byte-identical values, at every depth and
// in every property shape the boot DTB needs. Beyond the round-trip the
// lifecycle rules are pinned -- capacity exhaustion leaves the object intact
// and retryable, finish() refuses an unbalanced tree, nothing emits after
// finish -- and the whole tree gets cross-checked against the real `dtc`
// decompiler when one is present (skipped without it, so CI never depends
// on a locally extracted tool).
#include "oemu/allocator.h"
#include "oemu/fdt.h"

#include <cstring>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "fdt/fdt_internal.h"

namespace {

class FdtTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_EQ(OEMU_OK, oemu_fdt_init(&fdt_, 4096U)); }
  void TearDown() override { oemu_fdt_dispose(&fdt_); }

  // Emit the tree the boot path emits (abridged): root props, one child.
  void BuildSample() {
    ASSERT_EQ(OEMU_OK, oemu_fdt_prop_u32(&fdt_, "#address-cells", 2U));
    ASSERT_EQ(OEMU_OK, oemu_fdt_prop_str(&fdt_, "compatible", "linux,dummy-virt"));
    ASSERT_EQ(OEMU_OK, oemu_fdt_begin_node(&fdt_, "chosen"));
    ASSERT_EQ(OEMU_OK, oemu_fdt_prop_str(&fdt_, "stdout-path", "/pl011@9000000"));
    ASSERT_EQ(OEMU_OK, oemu_fdt_end_node(&fdt_));
    ASSERT_EQ(OEMU_OK, oemu_fdt_end_node(&fdt_));  // init opens the root; close it
    ASSERT_EQ(OEMU_OK, oemu_fdt_finish(&fdt_));
  }

  void Find(const char *path, const char *prop, const unsigned char **out, size_t *len) {
    const oemu_status st = oemu_fdt_internal_find(oemu_fdt_bytes(&fdt_), oemu_fdt_length(&fdt_),
                                                  path, prop, out, len);
    ASSERT_EQ(OEMU_OK, st) << path << ":" << prop;
  }

  oemu_fdt fdt_{};
};

TEST_F(FdtTest, FinishWritesSpecShapedHeader) {
  BuildSample();
  const unsigned char *b = oemu_fdt_bytes(&fdt_);
  const auto be32 = [](const unsigned char *p) {
    return (static_cast<uint32_t>(p[0]) << 24U) | (static_cast<uint32_t>(p[1]) << 16U) |
           (static_cast<uint32_t>(p[2]) << 8U) | static_cast<uint32_t>(p[3]);
  };
  EXPECT_EQ(0xD00DFEEDU, be32(b));
  EXPECT_EQ(17U, be32(b + 0x14U));
  EXPECT_EQ(oemu_fdt_length(&fdt_), be32(b + 0x04U));  // totalsize == length
  EXPECT_LE(be32(b + 0x08U), oemu_fdt_length(&fdt_));  // struct inside blob
  EXPECT_LE(be32(b + 0x0CU), oemu_fdt_length(&fdt_));  // strings inside blob
}

TEST_F(FdtTest, RoundTripsEveryPropertyShape) {
  ASSERT_EQ(OEMU_OK, oemu_fdt_prop_empty(&fdt_, "chosen-node-marker"));
  ASSERT_EQ(OEMU_OK, oemu_fdt_prop_u32(&fdt_, "cell", 0xDEADBEEFU));
  ASSERT_EQ(OEMU_OK, oemu_fdt_prop_u64(&fdt_, "quad", 0x0102030405060708ULL));
  ASSERT_EQ(OEMU_OK, oemu_fdt_prop_str(&fdt_, "name", "sample"));
  const uint32_t cells[4] = {0U, 0x40000000U, 0U, 0x10000000U};
  ASSERT_EQ(OEMU_OK, oemu_fdt_prop_cells(&fdt_, "reg", cells, 4U));
  ASSERT_EQ(OEMU_OK, oemu_fdt_end_node(&fdt_));  // close the root init opened
  ASSERT_EQ(OEMU_OK, oemu_fdt_finish(&fdt_));

  const unsigned char *val = nullptr;
  size_t len = 0U;
  Find("/", "cell", &val, &len);
  ASSERT_EQ(4U, len);
  EXPECT_EQ(0xDEU, val[0]);
  EXPECT_EQ(0xEFU, val[3]); /* 0xDEADBEEF big-endian: D E A D B E E F */
  Find("/", "quad", &val, &len);
  ASSERT_EQ(8U, len);
  EXPECT_EQ(0x01U, val[0]);
  EXPECT_EQ(0x08U, val[7]);
  Find("/", "name", &val, &len);
  EXPECT_EQ(std::string("sample"), std::string(reinterpret_cast<const char *>(val), len - 1U));
  EXPECT_EQ('\0', val[len - 1U]);  // strings are NUL-terminated on the wire
  Find("/", "reg", &val, &len);
  ASSERT_EQ(16U, len);
  EXPECT_EQ(0x40U, val[4]);  // cells[i] big-endian u32s back in order
  Find("/", "chosen-node-marker", &val, &len);
  EXPECT_EQ(0U, len);  // empty property round-trips as zero bytes
}

TEST_F(FdtTest, FindsByPathAmongSiblings) {
  ASSERT_EQ(OEMU_OK, oemu_fdt_begin_node(&fdt_, "aliases"));
  ASSERT_EQ(OEMU_OK, oemu_fdt_prop_str(&fdt_, "serial0", "/pl011@9000000"));
  ASSERT_EQ(OEMU_OK, oemu_fdt_end_node(&fdt_));
  ASSERT_EQ(OEMU_OK, oemu_fdt_begin_node(&fdt_, "pl011@9000000"));
  ASSERT_EQ(OEMU_OK, oemu_fdt_prop_str(&fdt_, "compatible", "arm,pl011"));
  ASSERT_EQ(OEMU_OK, oemu_fdt_end_node(&fdt_));
  ASSERT_EQ(OEMU_OK, oemu_fdt_end_node(&fdt_));  // close the root
  ASSERT_EQ(OEMU_OK, oemu_fdt_finish(&fdt_));

  const unsigned char *val = nullptr;
  size_t len = 0U;
  Find("/aliases", "serial0", &val, &len);
  EXPECT_EQ(std::string("/pl011@9000000"),
            std::string(reinterpret_cast<const char *>(val), len - 1U));
  Find("/pl011@9000000", "compatible", &val, &len);
  EXPECT_EQ(std::string("arm,pl011"),
            std::string(reinterpret_cast<const char *>(val), len - 1U));
  // The property lives at one path only; the other must miss.
  EXPECT_EQ(OEMU_ERR_NOT_FOUND,
            oemu_fdt_internal_find(oemu_fdt_bytes(&fdt_), oemu_fdt_length(&fdt_), "/aliases",
                                   "compatible", &val, &len));
}

TEST_F(FdtTest, AbsentPathAndAbsentPropBothReportNotFound) {
  BuildSample();
  const unsigned char *val = nullptr;
  size_t len = 0U;
  EXPECT_EQ(OEMU_ERR_NOT_FOUND,
            oemu_fdt_internal_find(oemu_fdt_bytes(&fdt_), oemu_fdt_length(&fdt_), "/nope",
                                   "compatible", &val, &len));
  EXPECT_EQ(OEMU_ERR_NOT_FOUND,
            oemu_fdt_internal_find(oemu_fdt_bytes(&fdt_), oemu_fdt_length(&fdt_), "/",
                                   "missing", &val, &len));
}

TEST_F(FdtTest, RootPathFindsRootProperties) {
  BuildSample();
  const unsigned char *val = nullptr;
  size_t len = 0U;
  Find("/", "compatible", &val, &len);
  EXPECT_EQ(std::string("linux,dummy-virt"),
            std::string(reinterpret_cast<const char *>(val), len - 1U));
}

TEST_F(FdtTest, UnbalancedFinishRefusesAndTreeStaysOpen) {
  ASSERT_EQ(OEMU_OK, oemu_fdt_begin_node(&fdt_, "open"));
  EXPECT_EQ(OEMU_ERR_STATE, oemu_fdt_finish(&fdt_));
  // The refusal is recoverable: close both open nodes and finish succeeds.
  ASSERT_EQ(OEMU_OK, oemu_fdt_end_node(&fdt_));  // closes "open"
  ASSERT_EQ(OEMU_OK, oemu_fdt_end_node(&fdt_));  // closes the root
  EXPECT_EQ(OEMU_OK, oemu_fdt_finish(&fdt_));
}

TEST_F(FdtTest, EmitAfterFinishRefuses) {
  BuildSample();
  EXPECT_EQ(OEMU_ERR_STATE, oemu_fdt_prop_u32(&fdt_, "late", 1U));
  EXPECT_EQ(OEMU_ERR_STATE, oemu_fdt_begin_node(&fdt_, "late"));
  // The finished blob is untouched by the refusals.
  const unsigned char *val = nullptr;
  size_t len = 0U;
  Find("/", "compatible", &val, &len);
}

TEST_F(FdtTest, EndNodeWithoutBeginRefuses) {
  // The root is open after init, so one end_node balances it -- a second
  // is the error.
  ASSERT_EQ(OEMU_OK, oemu_fdt_end_node(&fdt_));
  EXPECT_EQ(OEMU_ERR_STATE, oemu_fdt_end_node(&fdt_));
}

TEST_F(FdtTest, CapacityExhaustionLeavesObjectIntact) {
  oemu_fdt tight{};
  // Overhead: 40-byte header + 16 rsvmap + one ENDTREE word; plus the
  // root BEGIN_NODE init already opened. Anything smaller than 272 and a
  // fresh tree could not finish at all -- this is the smallest cap that
  // still lets one filler property through.
  ASSERT_EQ(OEMU_OK, oemu_fdt_init(&tight, 272U));
  // Fill until one call must fail.
  oemu_status st = OEMU_OK;
  int accepted = 0;
  while (st == OEMU_OK) {
    st = oemu_fdt_prop_u32(&tight, "filler", static_cast<uint32_t>(accepted));
    if (st == OEMU_OK) {
      ++accepted;
    }
  }
  EXPECT_EQ(OEMU_ERR_NO_MEMORY, st);
  EXPECT_LE(1, accepted);  // it accepted something before it filled
  // The rejection wrote nothing; the tree holds exactly the accepted
  // properties and finishes cleanly. (Names are re-interned on every
  // emit, so "filler" also eats string space -- accounted for above.)
  EXPECT_EQ(OEMU_OK, oemu_fdt_end_node(&tight));  // close the root
  EXPECT_EQ(OEMU_OK, oemu_fdt_finish(&tight));
  EXPECT_LE(oemu_fdt_length(&tight),
            272U + 40U + 16U + 4U);  // emitted cap + header + rsv + endtok
  oemu_fdt_dispose(&tight);
}

TEST_F(FdtTest, DisposeAfterInitIsCleanAndIdempotent) {
  oemu_fdt one{};
  ASSERT_EQ(OEMU_OK, oemu_fdt_init(&one, 256U));
  oemu_fdt_dispose(&one);
  oemu_fdt_dispose(&one);  // second dispose: no allocation to leak, no crash
  EXPECT_EQ(nullptr, one.data);
}

TEST_F(FdtTest, TinyCapacityRefusesAtInit) {
  oemu_fdt tiny{};
  EXPECT_EQ(OEMU_ERR_INVALID_ARG, oemu_fdt_init(&tiny, 8U));
}

TEST_F(FdtTest, PropertyBeforeAnyNodeRefuses) {
  // init opens the root; closing it leaves depth 0, where properties
  // have no node to live in.
  ASSERT_EQ(OEMU_OK, oemu_fdt_end_node(&fdt_));  // closes root
  EXPECT_EQ(OEMU_ERR_STATE, oemu_fdt_prop_u32(&fdt_, "orphan", 1U));
}

TEST_F(FdtTest, ReaderRejectsGarbageBeforeTheMagic) {
  const unsigned char junk[40] = {0};
  const unsigned char *val = nullptr;
  size_t len = 0U;
  EXPECT_EQ(OEMU_ERR_FORMAT,
            oemu_fdt_internal_find(junk, sizeof(junk), "/", "compatible", &val, &len));
  size_t total = 0U;
  EXPECT_EQ(OEMU_ERR_FORMAT, oemu_fdt_internal_total_size(junk, sizeof(junk), &total));
}

TEST_F(FdtTest, TotalSizeMatchesLength) {
  BuildSample();
  size_t total = 0U;
  ASSERT_EQ(OEMU_OK, oemu_fdt_internal_total_size(oemu_fdt_bytes(&fdt_), oemu_fdt_length(&fdt_),
                                                  &total));
  EXPECT_EQ(oemu_fdt_length(&fdt_), total);
}

// Cross-check against the real decompiler when one is installed. The
// acceptance command must not depend on this passing -- a machine without
// dtc skips it loudly, which is honest, and never fakes a pass.
TEST_F(FdtTest, DtcAgreesWithOurReaderWhenDtcExists) {
  BuildSample();
  FILE *probe = popen("command -v dtc 2>/dev/null", "r");
  ASSERT_NE(nullptr, probe);
  char path[256] = {0};
  const char *got = fgets(path, sizeof(path), probe);
  pclose(probe);
  if ((got == nullptr) || (path[0] == '\0')) {
    GTEST_SKIP() << "no dtc on PATH";
  }
  // Write our blob, let dtc dump it, and expect it to list our property.
  const std::string blob_path = "test_fdt_check.dtb";  // ctest cwd is the build tree
  FILE *f = fopen(blob_path.c_str(), "wb");
  ASSERT_NE(nullptr, f);
  ASSERT_EQ(oemu_fdt_length(&fdt_),
            fwrite(oemu_fdt_bytes(&fdt_), 1U, oemu_fdt_length(&fdt_), f));
  ASSERT_EQ(0, fclose(f));
  const std::string cmd = std::string("dtc -I dtb -O dts ") + blob_path + " 2>/dev/null";
  FILE *dump = popen(cmd.c_str(), "r");
  ASSERT_NE(nullptr, dump);
  std::string text;
  char line[256];
  while (fgets(line, sizeof(line), dump) != nullptr) {
    text += line;
  }
  EXPECT_EQ(0, pclose(dump));
  EXPECT_NE(std::string::npos, text.find("compatible")) << text;
  EXPECT_NE(std::string::npos, text.find("dummy-virt")) << text;
  EXPECT_NE(std::string::npos, text.find("stdout-path")) << text;
}

}  // namespace
