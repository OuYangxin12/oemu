/*
 * Black-box tests for the ELF loader.
 *
 * Images are assembled byte-by-byte via the shared support/elf_builder.h (no
 * external .elf), so a failure names exactly the header field that caused it, and
 * the malformed cases that a real assembler would never emit are as easy to write
 * as the well-formed ones. Encodings baked into the integration test's payload
 * were verified against the host assembler (see the repo's GAS-discipline note):
 *   mov x8,#94 = 0xd2800bc8   mov x0,#7 = 0xd28000e0   svc #0 = 0xd4000001
 */
#include "oemu/elf.h"
#include "oemu/exec.h"
#include "oemu/memory.h"
#include "oemu/sysenv.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <vector>

#include <gtest/gtest.h>

#include "support/elf_builder.h"
#include "support/tracking_allocator.h"

namespace {

using namespace oemu_test::elf;

class ElfTest : public ::testing::Test {
 protected:
  oemu_test::TrackingAllocator tracker_;
};

TEST_F(ElfTest, NullArgumentsAreRejected) {
  oemu_memory mem{};
  ASSERT_EQ(oemu_memory_init(&mem, 8U), OEMU_OK);
  std::vector<uint8_t> image = bare_header(0U);
  oemu_elf_image out{};
  EXPECT_EQ(oemu_elf_load(nullptr, image.data(), (uint64_t)image.size(), &out),
            OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(oemu_elf_load(&mem, nullptr, (uint64_t)image.size(), &out), OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(oemu_elf_load(&mem, image.data(), (uint64_t)image.size(), nullptr),
            OEMU_ERR_INVALID_ARG);
  oemu_memory_dispose(&mem);
  EXPECT_FALSE(tracker_.has_leaks());
}

TEST_F(ElfTest, TruncatedBelowHeaderIsInvalid) {
  oemu_memory mem{};
  ASSERT_EQ(oemu_memory_init(&mem, 8U), OEMU_OK);
  std::vector<uint8_t> image = bare_header(1U);
  image.resize(63U); /* one byte short of an ELF64 header */
  oemu_elf_image out{};
  EXPECT_EQ(oemu_elf_load(&mem, image.data(), (uint64_t)image.size(), &out),
            OEMU_ERR_INVALID_ARG);
  oemu_memory_dispose(&mem);
}

TEST_F(ElfTest, BadMagicIsInvalid) {
  oemu_memory mem{};
  ASSERT_EQ(oemu_memory_init(&mem, 8U), OEMU_OK);
  std::vector<uint8_t> image = bare_header(1U);
  image[0] = 0U;
  oemu_elf_image out{};
  EXPECT_EQ(oemu_elf_load(&mem, image.data(), (uint64_t)image.size(), &out),
            OEMU_ERR_INVALID_ARG);
  oemu_memory_dispose(&mem);
}

TEST_F(ElfTest, ForeignFormatIsUnsupported) {
  oemu_memory mem{};
  ASSERT_EQ(oemu_memory_init(&mem, 8U), OEMU_OK);
  oemu_elf_image out{};
  auto expect_unsupported = [&](std::function<void(std::vector<uint8_t> &)> mutate) {
    std::vector<uint8_t> image = bare_header(1U);
    mutate(image);
    EXPECT_EQ(oemu_elf_load(&mem, image.data(), (uint64_t)image.size(), &out),
              OEMU_ERR_UNSUPPORTED);
  };
  expect_unsupported([](std::vector<uint8_t> &v) { v[kOffEiClass] = 1U; });    /* ELF32 */
  expect_unsupported([](std::vector<uint8_t> &v) { v[kOffEiData] = 2U; });     /* big-endian */
  expect_unsupported([](std::vector<uint8_t> &v) { put16(v, kOffType, 3U); }); /* ET_DYN */
  expect_unsupported([](std::vector<uint8_t> &v) { put16(v, kOffMachine, 62U); }); /* x86-64 */
  oemu_memory_dispose(&mem);
  EXPECT_FALSE(tracker_.has_leaks());
}

TEST_F(ElfTest, ZeroSegmentsIsInvalid) {
  oemu_memory mem{};
  ASSERT_EQ(oemu_memory_init(&mem, 8U), OEMU_OK);
  std::vector<uint8_t> image = bare_header(0U);
  oemu_elf_image out{};
  EXPECT_EQ(oemu_elf_load(&mem, image.data(), (uint64_t)image.size(), &out),
            OEMU_ERR_INVALID_ARG);
  oemu_memory_dispose(&mem);
}

TEST_F(ElfTest, BadPhentsizeIsInvalid) {
  oemu_memory mem{};
  ASSERT_EQ(oemu_memory_init(&mem, 8U), OEMU_OK);
  std::vector<uint8_t> image = bare_header(1U);
  put16(image, kOffPhentsize, 32U);
  oemu_elf_image out{};
  EXPECT_EQ(oemu_elf_load(&mem, image.data(), (uint64_t)image.size(), &out),
            OEMU_ERR_INVALID_ARG);
  oemu_memory_dispose(&mem);
}

TEST_F(ElfTest, ProgramTableBeyondImageIsInvalid) {
  oemu_memory mem{};
  ASSERT_EQ(oemu_memory_init(&mem, 8U), OEMU_OK);
  /* Claims two entries but the buffer only holds the header + one phdr. */
  std::vector<uint8_t> image = build_image({SegmentSpec{0x400000U, to_bytes({0U}), 4U}}, 0U);
  put16(image, kOffPhnum, 2U);
  oemu_elf_image out{};
  EXPECT_EQ(oemu_elf_load(&mem, image.data(), (uint64_t)image.size(), &out),
            OEMU_ERR_INVALID_ARG);
  oemu_memory_dispose(&mem);
}

TEST_F(ElfTest, LoadsASingleSegment) {
  oemu_memory mem{};
  ASSERT_EQ(oemu_memory_init(&mem, 8U), OEMU_OK);
  const std::vector<uint8_t> payload = to_bytes({0x12345678U});
  std::vector<uint8_t> image =
      build_image({SegmentSpec{0x400000U, payload, (uint64_t)payload.size()}}, 0x400000U);
  oemu_elf_image out{};
  ASSERT_EQ(oemu_elf_load(&mem, image.data(), (uint64_t)image.size(), &out), OEMU_OK);
  EXPECT_EQ(out.entry, 0x400000U);
  EXPECT_EQ(out.segment_count, 1U);
  EXPECT_EQ(out.load_min, 0x400000U);
  EXPECT_EQ(out.load_max, 0x400004U);
  /* The mapped bytes must match the file slice exactly. */
  uint64_t word = 0U;
  ASSERT_EQ(oemu_memory_read(&mem, 0x400000U, OEMU_MEM_WORD, false, &word), OEMU_OK);
  EXPECT_EQ(word, 0x12345678U);
  oemu_memory_dispose(&mem);
  EXPECT_FALSE(tracker_.has_leaks());
}

TEST_F(ElfTest, TwoSegmentsWithBssTailAreZeroFilled) {
  oemu_memory mem{};
  ASSERT_EQ(oemu_memory_init(&mem, 8U), OEMU_OK);
  const std::vector<uint8_t> text = to_bytes({0xD2800BC8U, 0xD28000E0U, 0xD4000001U});
  const std::vector<uint8_t> data = to_bytes({0xAABBCCDDU});
  /* data maps 0x1000 bytes but carries only 4: the .bss tail must read back 0. */
  std::vector<uint8_t> image =
      build_image({SegmentSpec{0x400000U, text, (uint64_t)text.size(), kFlagRx},
                   SegmentSpec{0x800000U, data, 0x1000U, 6U /* PF_R|PF_W */}},
                  0x400000U);
  oemu_elf_image out{};
  ASSERT_EQ(oemu_elf_load(&mem, image.data(), (uint64_t)image.size(), &out), OEMU_OK);
  EXPECT_EQ(out.segment_count, 2U);
  EXPECT_EQ(out.load_min, 0x400000U);
  EXPECT_EQ(out.load_max, 0x801000U);
  uint64_t init = 0U;
  uint64_t tail = 0xDEADBEEFU;
  ASSERT_EQ(oemu_memory_read(&mem, 0x800000U, OEMU_MEM_WORD, false, &init), OEMU_OK);
  ASSERT_EQ(oemu_memory_read(&mem, 0x800008U, OEMU_MEM_WORD, false, &tail), OEMU_OK);
  EXPECT_EQ(init, 0xAABBCCDDU);
  EXPECT_EQ(tail, 0U); /* the zero-filled bss */
  oemu_memory_dispose(&mem);
  EXPECT_FALSE(tracker_.has_leaks());
}

TEST_F(ElfTest, IgnoresNonLoadProgramHeaders) {
  /* A PT_NOTE alongside a PT_LOAD: only the load segment is mapped, and the
   * note is stepped over without disturbing the count. */
  oemu_memory mem{};
  ASSERT_EQ(oemu_memory_init(&mem, 8U), OEMU_OK);
  const std::vector<uint8_t> note = to_bytes({0U, 0U});
  const std::vector<uint8_t> code = to_bytes({0xD4000001U});
  SegmentSpec note_spec{0U, note, 8U, 4U, kTypeNote, 1U};
  SegmentSpec load_spec{0x400000U, code, (uint64_t)code.size()};
  std::vector<uint8_t> image = build_image({note_spec, load_spec}, 0x400000U);
  oemu_elf_image out{};
  ASSERT_EQ(oemu_elf_load(&mem, image.data(), (uint64_t)image.size(), &out), OEMU_OK);
  EXPECT_EQ(out.segment_count, 1U);
  uint32_t word = 0U;
  EXPECT_EQ(oemu_memory_fetch32(&mem, 0x400000U, &word), OEMU_OK);
  oemu_memory_dispose(&mem);
  EXPECT_FALSE(tracker_.has_leaks());
}

TEST_F(ElfTest, ZeroMemszLoadSegmentIsNothingToLoad) {
  /* A PT_LOAD declaring zero memory size is skipped; if it is the only segment
   * the image has nothing to load and is rejected. */
  oemu_memory mem{};
  ASSERT_EQ(oemu_memory_init(&mem, 8U), OEMU_OK);
  std::vector<uint8_t> image = build_image({SegmentSpec{0x400000U, {}, 0U}}, 0x400000U);
  oemu_elf_image out{};
  EXPECT_EQ(oemu_elf_load(&mem, image.data(), (uint64_t)image.size(), &out),
            OEMU_ERR_INVALID_ARG);
  oemu_memory_dispose(&mem);
  EXPECT_FALSE(tracker_.has_leaks());
}

TEST_F(ElfTest, SegmentPermissionsFollowTheFlagBits) {
  oemu_memory mem{};
  ASSERT_EQ(oemu_memory_init(&mem, 8U), OEMU_OK);
  /* PF_R only: fetching an instruction from it must fault (no EXEC). */
  std::vector<uint8_t> image = build_image(
      {SegmentSpec{0x400000U, to_bytes({0xD4000001U}), 4U, 4U /* PF_R */}}, 0x400000U);
  oemu_elf_image out{};
  ASSERT_EQ(oemu_elf_load(&mem, image.data(), (uint64_t)image.size(), &out), OEMU_OK);
  uint64_t word = 0U;
  uint32_t fword = 0U;
  EXPECT_EQ(oemu_memory_fetch32(&mem, 0x400000U, &fword), OEMU_ERR_FAULT);
  EXPECT_EQ(oemu_memory_read(&mem, 0x400000U, OEMU_MEM_WORD, false, &word), OEMU_OK);
  EXPECT_EQ(oemu_memory_write(&mem, 0x400000U, OEMU_MEM_WORD, 0U), OEMU_ERR_FAULT); /* no W */
  oemu_memory_dispose(&mem);
  EXPECT_FALSE(tracker_.has_leaks());
}

TEST_F(ElfTest, OverlappingSegmentsAreRejectedWithoutTouchingMemory) {
  oemu_memory mem{};
  ASSERT_EQ(oemu_memory_init(&mem, 8U), OEMU_OK);
  const std::size_t before = tracker_.alloc_count();
  std::vector<uint8_t> image = build_image({SegmentSpec{0x400000U, to_bytes({1U}), 0x100U},
                                            SegmentSpec{0x400080U, to_bytes({2U}), 0x100U}},
                                           0x400000U);
  oemu_elf_image out{};
  EXPECT_EQ(oemu_elf_load(&mem, image.data(), (uint64_t)image.size(), &out), OEMU_ERR_RANGE);
  /* No backing block was mapped for a rejected image: only the temp array came
   * and went, so the allocation count moved by exactly the one temp alloc. */
  EXPECT_EQ(tracker_.alloc_count() - before, 1U);
  oemu_memory_dispose(&mem);
  EXPECT_FALSE(tracker_.has_leaks());
}

TEST_F(ElfTest, FileSizeLargerThanMemorySizeIsInvalid) {
  oemu_memory mem{};
  ASSERT_EQ(oemu_memory_init(&mem, 8U), OEMU_OK);
  /* memsz deliberately smaller than the 4-byte payload. */
  std::vector<uint8_t> image = build_image({SegmentSpec{0x400000U, to_bytes({1U}), 2U}}, 0U);
  oemu_elf_image out{};
  EXPECT_EQ(oemu_elf_load(&mem, image.data(), (uint64_t)image.size(), &out),
            OEMU_ERR_INVALID_ARG);
  oemu_memory_dispose(&mem);
}

TEST_F(ElfTest, ZeroAlignIsInvalid) {
  oemu_memory mem{};
  ASSERT_EQ(oemu_memory_init(&mem, 8U), OEMU_OK);
  std::vector<uint8_t> image =
      build_image({SegmentSpec{0x400000U, to_bytes({1U}), 4U, kFlagRx, 0U}}, 0U);
  oemu_elf_image out{};
  EXPECT_EQ(oemu_elf_load(&mem, image.data(), (uint64_t)image.size(), &out),
            OEMU_ERR_INVALID_ARG);
  oemu_memory_dispose(&mem);
}

TEST_F(ElfTest, NoPermissionBitsIsInvalid) {
  oemu_memory mem{};
  ASSERT_EQ(oemu_memory_init(&mem, 8U), OEMU_OK);
  std::vector<uint8_t> image =
      build_image({SegmentSpec{0x400000U, to_bytes({1U}), 4U, 0U /* no flags */}}, 0U);
  oemu_elf_image out{};
  EXPECT_EQ(oemu_elf_load(&mem, image.data(), (uint64_t)image.size(), &out),
            OEMU_ERR_INVALID_ARG);
  oemu_memory_dispose(&mem);
}

TEST_F(ElfTest, SegmentAddressWrapIsOverflow) {
  oemu_memory mem{};
  ASSERT_EQ(oemu_memory_init(&mem, 8U), OEMU_OK);
  /* vaddr + memsz must not wrap: vaddr near the top with a memsz that crosses
   * 2^64. Built by hand so no assembler is required. */
  std::vector<uint8_t> image = build_image({SegmentSpec{0U, to_bytes({1U}), 4U}}, UINT64_C(1));
  const uint64_t ph = kEHdrSize;
  put64(image, ph + kPhVaddr, UINT64_C(0xFFFFFFFFFFFFFFFF));
  put64(image, ph + kPhMemsz, 0x1000U);
  oemu_elf_image out{};
  EXPECT_EQ(oemu_elf_load(&mem, image.data(), (uint64_t)image.size(), &out), OEMU_ERR_OVERFLOW);
  oemu_memory_dispose(&mem);
}

TEST_F(ElfTest, TooManySegmentsForTheRegionTableIsRange) {
  /* Capacity 1 but the image has two disjoint load segments. */
  oemu_memory mem{};
  ASSERT_EQ(oemu_memory_init(&mem, 1U), OEMU_OK);
  std::vector<uint8_t> image = build_image(
      {SegmentSpec{0x400000U, to_bytes({1U}), 4U}, SegmentSpec{0x800000U, to_bytes({2U}), 4U}},
      0x400000U);
  oemu_elf_image out{};
  EXPECT_EQ(oemu_elf_load(&mem, image.data(), (uint64_t)image.size(), &out), OEMU_ERR_RANGE);
  oemu_memory_dispose(&mem);
  EXPECT_FALSE(tracker_.has_leaks());
}

TEST_F(ElfTest, BackingAllocationFailureReportsNoMemory) {
  /* oemu_memory captures its allocator at init, so the failure must be armed
   * before memory_init or the region's backing block never sees it. With the
   * double installed first: #1 is the region table, #2 the loader's temp array,
   * #3 the first segment's backing block -- failing #3 lands the failure
   * mid-map, where dispose-and-retry is the documented contract. */
  std::vector<uint8_t> image =
      build_image({SegmentSpec{0x400000U, to_bytes({0xD4000001U}), 4U}}, 0x400000U);
  oemu_elf_image out{};
  {
    oemu_test::FailingAllocator failing(3U);
    oemu_memory mem{};
    ASSERT_EQ(oemu_memory_init(&mem, 8U), OEMU_OK);
    EXPECT_EQ(oemu_elf_load(&mem, image.data(), (uint64_t)image.size(), &out),
              OEMU_ERR_NO_MEMORY);
    /* Dispose inside the scope: the model still holds the failing allocator. */
    oemu_memory_dispose(&mem);
  }
}

TEST_F(ElfTest, TempArrayAllocationFailureReportsNoMemoryBeforeMapping) {
  /* Failing the loader's temp array (#2, just after the region table) proves the
   * whole validation phase is skipped on allocation failure and nothing is
   * mapped -- the clean half of the OOM contract. */
  std::vector<uint8_t> image =
      build_image({SegmentSpec{0x400000U, to_bytes({0xD4000001U}), 4U}}, 0x400000U);
  oemu_elf_image out{};
  {
    oemu_test::FailingAllocator failing(2U);
    oemu_memory mem{};
    ASSERT_EQ(oemu_memory_init(&mem, 8U), OEMU_OK);
    EXPECT_EQ(oemu_elf_load(&mem, image.data(), (uint64_t)image.size(), &out),
              OEMU_ERR_NO_MEMORY);
    oemu_memory_dispose(&mem);
  }
}

/*
 * The load-bearing integration case: a real (if minimal) program put through
 * the loader, entered, and run to completion. If the loader mapped the wrong
 * address, got the entry wrong, or mis-set permissions, this does not reach the
 * expected exit code -- and it needs no cross toolchain.
 */
TEST_F(ElfTest, LoadedProgramRunsAndExitsWithItsStatusCode) {
  oemu_memory mem{};
  ASSERT_EQ(oemu_memory_init(&mem, 8U), OEMU_OK);
  /* exit_group(7): mov x8,#94 ; mov x0,#7 ; svc #0 */
  const std::vector<uint8_t> code = to_bytes({0xD2800BC8U, 0xD28000E0U, 0xD4000001U});
  std::vector<uint8_t> image =
      build_image({SegmentSpec{0x400000U, code, (uint64_t)code.size(), kFlagRx}}, 0x400000U);
  oemu_elf_image img{};
  ASSERT_EQ(oemu_elf_load(&mem, image.data(), (uint64_t)image.size(), &img), OEMU_OK);
  ASSERT_EQ(img.entry, 0x400000U);
  ASSERT_EQ(img.segment_count, 1U);

  /* The loader does not synthesise a stack; the caller does. */
  const uint64_t stack = 0x600000U;
  ASSERT_EQ(oemu_memory_map(&mem, stack, 0x1000U, OEMU_PERM_READ | OEMU_PERM_WRITE), OEMU_OK);

  oemu_cpu cpu{};
  oemu_sysenv env{};
  std::FILE *out = std::tmpfile();
  ASSERT_NE(out, nullptr);
  oemu_sysenv_init(&env, out);
  ASSERT_EQ(oemu_cpu_init(&cpu, img.entry, stack + 0x800U), OEMU_OK);

  uint64_t completed = 0U;
  EXPECT_EQ(oemu_exec_run(&cpu, &mem, &env, 1000U, &completed), OEMU_OK);
  EXPECT_TRUE(oemu_sysenv_exited(&env));
  EXPECT_EQ(oemu_sysenv_exit_code(&env), 7);
  EXPECT_GE(completed, 3U);

  std::fclose(out);
  oemu_memory_dispose(&mem);
  EXPECT_FALSE(tracker_.has_leaks());
}

}  // namespace
