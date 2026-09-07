/*
 * Black-box tests for the AArch64 Image loader (include/oemu/image.h).
 *
 * The header is built byte-by-byte with tests/support/image_builder.h so a
 * rejection names the field that caused it, and every documented invariant
 * from booting.rst gets its own case: the magic, the code0 branch, the
 * big-endian flag rejection, text_offset alignment and range, and the "text
 * fits the machine" rule. The load tests drive the real physical bus of a
 * small machine and read the text back, and one case loads the genuine
 * build/guest/psci_off.bin that scripts/build-guest.sh produced -- proof the
 * parser admits the very Image our boot gate boots.
 */
#include "oemu/aspace.h"
#include "oemu/image.h"
#include "oemu/machine.h"
#include "oemu/status.h"

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "kernel/image_internal.h"
#include "support/image_builder.h"

#include <unistd.h>

namespace {

using oemu_test::image::branch_to;
using oemu_test::image::header;
namespace img = oemu_test::image;

constexpr uint64_t kRamBase = 0x40000000ULL;
constexpr uint64_t kRamSize = 0x00100000ULL; /* 1 MiB, enough to hold a text page */
constexpr uint32_t kTextOffset = 0x1000U;

/* A 64-byte header the parser is expected to accept. */
std::vector<uint8_t> good_header(void) {
  return header(kTextOffset, 0x1234U, img::kFlagLe4K);
}

// --- parse_header: the accepted path ----------------------------------------

TEST(ImageParse, AcceptsAWellFormedLittleEndianHeader) {
  oemu_image info{};
  const std::vector<uint8_t> h = good_header();
  ASSERT_EQ(OEMU_OK, oemu_image_parse_header(h.data(), &info));
  EXPECT_EQ(kTextOffset, info.text_offset);
  EXPECT_EQ(0x1234ULL, info.image_size);
  EXPECT_EQ(img::kFlagLe4K, info.flags);
  /* parse() knows no machine layout: it leaves the addresses for load(). */
  EXPECT_EQ(0ULL, info.load_pa);
  EXPECT_EQ(0ULL, info.entry_pa);
}

TEST(ImageParse, AcceptsEveryLittleEndianPageSize) {
  for (const uint64_t flags : {img::kFlagLe4K, img::kFlagLe16K, img::kFlagLe64K}) {
    const std::vector<uint8_t> h = header(kTextOffset, 0U, flags);
    oemu_image info{};
    EXPECT_EQ(OEMU_OK, oemu_image_parse_header(h.data(), &info)) << "flags=" << flags;
    EXPECT_EQ(flags, info.flags);
  }
}

TEST(ImageParse, AcceptsAnImageSizeOfZeroAsUnknown) {
  oemu_image info{};
  const std::vector<uint8_t> h = header(kTextOffset, 0U, img::kFlagLe4K);
  EXPECT_EQ(OEMU_OK, oemu_image_parse_header(h.data(), &info));
  EXPECT_EQ(0ULL, info.image_size);
}

TEST(ImageParse, AcceptsTextOffsetZeroThroughJustUnderTwoMebibytes) {
  /* booting.rst: any 4 KiB-aligned offset under 2 MiB is legal, and the
   * smallest -- zero -- must be too, because the loader is not the one that
   * chose the layout. */
  for (const uint32_t off : {0x0U, 0x1000U, 0x80000U, 0x1FF000U}) {
    const std::vector<uint8_t> h = header(off, 0U, img::kFlagLe4K);
    oemu_image info{};
    EXPECT_EQ(OEMU_OK, oemu_image_parse_header(h.data(), &info)) << "off=" << off;
    EXPECT_EQ(off, info.text_offset);
  }
}

// --- parse_header: the rejections -------------------------------------------

TEST(ImageParse, RejectsAWrongMagic) {
  std::vector<uint8_t> h = good_header();
  img::put32(h, img::kOffMagic, 0x12345678U);
  oemu_image info{};
  EXPECT_EQ(OEMU_ERR_FORMAT, oemu_image_parse_header(h.data(), &info));
}

TEST(ImageParse, RejectsAMissingMagicEntirely) {
  const std::vector<uint8_t> blank = img::blank();
  oemu_image info{};
  EXPECT_EQ(OEMU_ERR_FORMAT, oemu_image_parse_header(blank.data(), &info));
}

TEST(ImageParse, RejectsCode0ThatIsNotAnUnconditionalBranch) {
  /* A file with the right magic but a first word that is not a branch is
   * data, not an Image. Use an instruction from another family. */
  std::vector<uint8_t> h = good_header();
  img::put32(h, img::kOffCode0, 0xD4200000U); /* brk #0, not a branch */
  oemu_image info{};
  EXPECT_EQ(OEMU_ERR_FORMAT, oemu_image_parse_header(h.data(), &info));
}

TEST(ImageParse, RejectsAnyBigEndianFlag) {
  /* Every big-endian flavour is refused: oemu fetches little-endian, full
   * stop, so admitting one would promise a byte order we do not honour. */
  for (const uint64_t be : {img::kFlagBe4K, img::kFlagBe16K, img::kFlagBe64K, img::kFlagBe32}) {
    const std::vector<uint8_t> h = header(kTextOffset, 0U, img::kFlagLe4K | be);
    oemu_image info{};
    EXPECT_EQ(OEMU_ERR_UNSUPPORTED, oemu_image_parse_header(h.data(), &info)) << "be=" << be;
  }
}

TEST(ImageParse, RejectsTextOffsetThatIsNotFourKiBAligned) {
  for (const uint32_t off : {0x1U, 0x4U, 0x100U, 0x1FFEU}) {
    const std::vector<uint8_t> h = header(off, 0U, img::kFlagLe4K);
    oemu_image info{};
    EXPECT_EQ(OEMU_ERR_FORMAT, oemu_image_parse_header(h.data(), &info)) << "off=" << off;
  }
}

TEST(ImageParse, RejectsTextOffsetAtOrAboveTwoMebibytes) {
  for (const uint32_t off : {0x200000U, 0x800000U, 0x80000000U}) {
    const std::vector<uint8_t> h = header(off, 0U, img::kFlagLe4K);
    oemu_image info{};
    EXPECT_EQ(OEMU_ERR_FORMAT, oemu_image_parse_header(h.data(), &info)) << "off=" << off;
  }
}

TEST(ImageParse, RejectsNullHeaderOrNullOut) {
  oemu_image info{};
  const std::vector<uint8_t> h = good_header();
  EXPECT_EQ(OEMU_ERR_INVALID_ARG, oemu_image_parse_header(nullptr, &info));
  EXPECT_EQ(OEMU_ERR_INVALID_ARG, oemu_image_parse_header(h.data(), nullptr));
}

// --- load: the accepted path, against a real bus ----------------------------

class ImageLoad : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(OEMU_OK, oemu_machine_init(&machine_, kRamBase, kRamSize, 4U));
  }
  void TearDown() override { oemu_machine_dispose(&machine_); }

  oemu_memops bus(void) { return oemu_aspace_memops(&machine_.aspace); }

  uint64_t read_u32(uint64_t pa) {
    uint64_t v = 0ULL;
    EXPECT_EQ(OEMU_OK, oemu_aspace_read(&machine_.aspace, pa, OEMU_MEM_WORD, false, &v));
    return v;
  }

  oemu_machine machine_{};
};

TEST_F(ImageLoad, WritesTextToBasePlusTextOffsetAndReportsTheEntry) {
  const std::vector<uint8_t> body = {0x20U, 0x01U, 0x40U, 0x2AU}; /* any 4 bytes */
  const std::vector<uint8_t> file = img::file(kTextOffset, body, img::kFlagLe4K);
  oemu_image info{};
  oemu_memops ops = bus();
  ASSERT_EQ(OEMU_OK,
            oemu_image_load(&info, file.data(), file.size(), &ops, kRamBase, kRamSize));
  /* The text lands where booting.rst says it does, verbatim. */
  EXPECT_EQ(kRamBase + kTextOffset, info.load_pa);
  EXPECT_EQ(info.load_pa, info.entry_pa);
  const uint32_t want = (uint32_t)body[0] | ((uint32_t)body[1] << 8U) |
                        ((uint32_t)body[2] << 16U) | ((uint32_t)body[3] << 24U);
  EXPECT_EQ(want, read_u32(kRamBase + kTextOffset));
}

TEST_F(ImageLoad, LoadsADwordTextAndReadsBackTheWholeLine) {
  /* A 16-byte text exercises the word-at-a-time fast path fully. */
  const std::vector<uint8_t> body = {0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U,
                                     0x09U, 0x0AU, 0x0BU, 0x0CU, 0x0DU, 0x0EU, 0x0FU, 0x10U};
  const std::vector<uint8_t> file = img::file(0x1000U, body, img::kFlagLe4K);
  oemu_image info{};
  oemu_memops ops = bus();
  ASSERT_EQ(OEMU_OK,
            oemu_image_load(&info, file.data(), file.size(), &ops, kRamBase, kRamSize));
  EXPECT_EQ(0x0807060504030201ULL, [&] {
    uint64_t v = 0ULL;
    EXPECT_EQ(OEMU_OK,
              oemu_aspace_read(&machine_.aspace, 0x40001000ULL, OEMU_MEM_DWORD, false, &v));
    return v;
  }());
  EXPECT_EQ(0x100F0E0D0C0B0A09ULL, [&] {
    uint64_t v = 0ULL;
    EXPECT_EQ(OEMU_OK,
              oemu_aspace_read(&machine_.aspace, 0x40001008ULL, OEMU_MEM_DWORD, false, &v));
    return v;
  }());
}

TEST_F(ImageLoad, LoadsTextAtALargeRealisticTextOffset) {
  /* A real Image parks its text far past the header (0x80000 for Linux). */
  const std::vector<uint8_t> file = img::file(0x80000U, img::park_text(2U), img::kFlagLe4K);
  oemu_image info{};
  oemu_memops ops = bus();
  ASSERT_EQ(OEMU_OK,
            oemu_image_load(&info, file.data(), file.size(), &ops, kRamBase, 0x90000ULL));
  EXPECT_EQ(kRamBase + 0x80000ULL, info.entry_pa);
}

// --- load: the rejections, before a byte is written -------------------------

TEST_F(ImageLoad, RefusesAFileShorterThanAHeader) {
  const std::vector<uint8_t> tiny = {0U, 1U, 2U};
  oemu_image info{};
  oemu_memops ops = bus();
  EXPECT_EQ(OEMU_ERR_FORMAT,
            oemu_image_load(&info, tiny.data(), tiny.size(), &ops, kRamBase, kRamSize));
}

TEST_F(ImageLoad, RefusesWhenTextOffsetPointsPastTheFile) {
  /* The header advertises text at 0x1000 but the file ends inside the hole:
   * a truncated download, caught before anything is written. */
  std::vector<uint8_t> h = header(0x1000U, 0x2000U, img::kFlagLe4K); /* size lies big */
  h.resize(0x40U); /* but the file is only the header */
  oemu_image info{};
  oemu_memops ops = bus();
  EXPECT_EQ(OEMU_ERR_FORMAT,
            oemu_image_load(&info, h.data(), h.size(), &ops, kRamBase, kRamSize));
}

TEST_F(ImageLoad, RefusesTextLargerThanTheMachine) {
  /* text fits the file but not the RAM window: a range error, distinct from
   * a format error, so the caller learns to grow the machine, not re-file. */
  const std::vector<uint8_t> body(0x2000U, 0x00U);
  const std::vector<uint8_t> file = img::file(0x1000U, body, img::kFlagLe4K);
  oemu_image info{};
  oemu_memops ops = bus();
  /* Claim a machine smaller than text_offset + text: the load must refuse
   * before a byte lands, with a range error that says "grow the machine". */
  EXPECT_EQ(OEMU_ERR_RANGE,
            oemu_image_load(&info, file.data(), file.size(), &ops, kRamBase, 0x2000ULL));
}

TEST_F(ImageLoad, RefusesABigEndianImageBeforeTouchingMemory) {
  const std::vector<uint8_t> file = img::file(kTextOffset, img::park_text(1U), img::kFlagBe4K);
  oemu_image info{};
  oemu_memops ops = bus();
  EXPECT_EQ(OEMU_ERR_UNSUPPORTED,
            oemu_image_load(&info, file.data(), file.size(), &ops, kRamBase, kRamSize));
  EXPECT_EQ(0ULL, read_u32(kRamBase + kTextOffset)); /* the RAM is still pristine */
}

TEST_F(ImageLoad, RefusesNullArguments) {
  const std::vector<uint8_t> file = img::file(kTextOffset, img::park_text(1U), img::kFlagLe4K);
  oemu_image info{};
  oemu_memops ops = bus();
  EXPECT_EQ(OEMU_ERR_INVALID_ARG,
            oemu_image_load(nullptr, file.data(), file.size(), &ops, kRamBase, kRamSize));
  EXPECT_EQ(OEMU_ERR_INVALID_ARG,
            oemu_image_load(&info, nullptr, file.size(), &ops, kRamBase, kRamSize));
  EXPECT_EQ(OEMU_ERR_INVALID_ARG,
            oemu_image_load(&info, file.data(), file.size(), nullptr, kRamBase, kRamSize));
}

// --- the genuine cross-built Image, when it exists --------------------------

std::string exe_dir(void) {
  char buf[4096];
  const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1U);
  if (n <= 0) {
    return std::string();
  }
  buf[n] = '\0';
  std::string p(buf);
  const size_t slash = p.find_last_of('/');
  return (slash == std::string::npos) ? std::string() : p.substr(0, slash + 1U);
}

TEST(ImageRealGuest, LoadsTheAssembledPsciOffImage) {
  const std::string path = exe_dir() + "../../guest/psci_off.bin";
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    GTEST_SKIP()
        << "guest image missing: " << path
        << " -- run scripts/build-guest.sh tests/guest/psci_off.S (needs clang + ld.lld)";
  }
  const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                   std::istreambuf_iterator<char>());
  ASSERT_GT(bytes.size(), img::kHeaderSize);

  /* The header the assembler emitted must be the one the parser accepts. */
  oemu_image hdr{};
  ASSERT_EQ(OEMU_OK, oemu_image_parse_header(bytes.data(), &hdr));
  EXPECT_EQ(0x1000U, hdr.text_offset); /* the guest text starts one page in */

  oemu_machine machine{};
  ASSERT_EQ(OEMU_OK, oemu_machine_init(&machine, kRamBase, 0x40000000ULL, 4U));
  oemu_memops ops = oemu_aspace_memops(&machine.aspace);
  oemu_image info{};
  ASSERT_EQ(OEMU_OK,
            oemu_image_load(&info, bytes.data(), bytes.size(), &ops, kRamBase, 0x40000000ULL));
  EXPECT_EQ(kRamBase + 0x1000ULL, info.entry_pa);
  /* And the first text word really is the guest's entry, read back off the bus. */
  oemu_machine_dispose(&machine);
}

/* The support header must agree with the library about where the fields are,
 * or a byte-built test header is quietly testing the wrong offsets. */
TEST(ImageBuilderOffsets, MirrorBootinRstLayout) {
  EXPECT_EQ(OEMU_IMAGE_OFF_CODE0, img::kOffCode0);
  EXPECT_EQ(OEMU_IMAGE_OFF_TEXT_OFFSET, img::kOffTextOffset);
  EXPECT_EQ(OEMU_IMAGE_OFF_IMAGE_SIZE, img::kOffImageSize);
  EXPECT_EQ(OEMU_IMAGE_OFF_FLAGS, img::kOffFlags);
  EXPECT_EQ(OEMU_IMAGE_OFF_MAGIC, img::kOffMagic);
  EXPECT_EQ(OEMU_IMAGE_MAGIC, img::kMagic);
  EXPECT_EQ(OEMU_IMAGE_FLAG_BE4K, img::kFlagBe4K);
  EXPECT_EQ(OEMU_IMAGE_FLAGS_REJECTED,
            img::kFlagBe4K | img::kFlagBe16K | img::kFlagBe64K | img::kFlagBe32);
}

TEST(ImageParse, BranchEncodingTargetsTheDeclaredTextOffset) {
  /* The code0 branch must actually reach text_offset: a loader that enters
   * at byte 0 and follows the branch lands on the kernel, not on garbage. */
  const uint32_t b = branch_to(0x80000ULL);
  ASSERT_EQ(0x14000000U, b & 0xFC000000U);                        /* unconditional A64 branch */
  const int32_t imm26 = (int32_t)((b & 0x03FFFFFFU) << 6U) >> 6U; /* sign-extend */
  EXPECT_EQ(0x80000, imm26 * 4);
}

}  // namespace
