/*
 * Tests for the four-syscall SVC surface. The guest-facing contract is Linux's,
 * so every expectation below is stated as a Linux errno, and the guest pointer
 * checks are the interesting part: a syscall must never be able to read or
 * write outside what the memory model allows.
 */
#include "oemu/memory.h"
#include "oemu/sysenv.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include "support/tracking_allocator.h"

namespace {

constexpr uint64_t kData = UINT64_C(0x100000);
constexpr uint64_t kRegion = UINT64_C(0x1000);

class SysenvTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(oemu_memory_init(&mem_, 8U), OEMU_OK);
    ASSERT_EQ(oemu_memory_map(&mem_, kData, kRegion, OEMU_PERM_READ | OEMU_PERM_WRITE),
              OEMU_OK);
    out_ = tmpfile();
    ASSERT_NE(out_, nullptr);
    oemu_sysenv_init(&env_, out_);
  }
  void TearDown() override {
    if (out_ != nullptr) {
      std::fclose(out_);
    }
    oemu_memory_dispose(&mem_);
    EXPECT_FALSE(tracker_.has_leaks());
  }

  /* Puts `bytes` at guest address `gva` so write() has something to send. */
  void fill_guest(uint64_t gva, const std::string &bytes) {
    ASSERT_EQ(oemu_memory_write_bytes(
                  &mem_, gva, reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size()),
              OEMU_OK);
  }

  std::string captured() {
    std::fflush(out_);
    std::rewind(out_);
    std::string text;
    std::array<char, 256> buf{};
    size_t got;
    while ((got = std::fread(buf.data(), 1U, buf.size(), out_)) > 0U) {
      text.append(buf.data(), got);
    }
    std::fseek(out_, 0L, SEEK_END); /* let further writes append */
    return text;
  }

  int64_t call(uint64_t nr, uint64_t a0 = 0U, uint64_t a1 = 0U, uint64_t a2 = 0U,
               uint64_t a3 = 0U) {
    const uint64_t args[6] = {a0, a1, a2, a3, 0U, 0U};
    return oemu_sysenv_syscall(&env_, &mem_, nr, args);
  }

  oemu_memory mem_{};
  oemu_sysenv env_{};
  FILE *out_ = nullptr;
  oemu_test::TrackingAllocator tracker_;
};

TEST(SysenvInit, NullEnvironmentIsARenamingOfNothing) {
  oemu_sysenv_init(nullptr, nullptr); /* must not crash */
  oemu_sysenv env{};
  oemu_sysenv_init(&env, stdout);
  EXPECT_FALSE(oemu_sysenv_exited(&env));
  EXPECT_EQ(oemu_sysenv_exit_code(&env), 0);
  EXPECT_FALSE(oemu_sysenv_exited(nullptr));
  EXPECT_EQ(oemu_sysenv_exit_code(nullptr), 0);
}

TEST_F(SysenvTest, SyscallRefusesNullPointers) {
  const uint64_t args[6] = {0U, 0U, 0U, 0U, 0U, 0U};
  EXPECT_EQ(oemu_sysenv_syscall(nullptr, &mem_, OEMU_SYS_EXIT, args), -OEMU_EINVAL);
  EXPECT_EQ(oemu_sysenv_syscall(&env_, &mem_, OEMU_SYS_EXIT, nullptr), -OEMU_EINVAL);
}

TEST_F(SysenvTest, UnknownSyscallIsEnosys) {
  EXPECT_EQ(call(0U), -OEMU_ENOSYS); /* io_setup: not offered */
  EXPECT_EQ(call(OEMU_SYS_WRITE + 1U), -OEMU_ENOSYS);
  EXPECT_EQ(call(UINT64_C(0xFFFFFFFFFFFF)), -OEMU_ENOSYS);
  EXPECT_FALSE(oemu_sysenv_exited(&env_)); /* a refusal is not a shutdown */
}

TEST_F(SysenvTest, ExitAndExitGroupRecordTheCode) {
  EXPECT_EQ(call(OEMU_SYS_EXIT, 3U), 0);
  EXPECT_TRUE(oemu_sysenv_exited(&env_));
  EXPECT_EQ(oemu_sysenv_exit_code(&env_), 3);

  oemu_sysenv_init(&env_, out_); /* re-arm */
  EXPECT_FALSE(oemu_sysenv_exited(&env_));
  EXPECT_EQ(call(OEMU_SYS_EXIT_GROUP, 0U), 0);
  EXPECT_TRUE(oemu_sysenv_exited(&env_));
  EXPECT_EQ(oemu_sysenv_exit_code(&env_), 0);
}

TEST_F(SysenvTest, ExitCodeIsTruncatedLikeLinuxTruncatesIt) {
  /* Linux keeps the low 32 bits of x0; -1 comes back as -1. */
  EXPECT_EQ(call(OEMU_SYS_EXIT, UINT64_C(0xFFFFFFFF12345678)), 0);
  EXPECT_EQ(oemu_sysenv_exit_code(&env_), 0x12345678);
  oemu_sysenv_init(&env_, out_);
  EXPECT_EQ(call(OEMU_SYS_EXIT, UINT64_MAX), 0);
  EXPECT_EQ(oemu_sysenv_exit_code(&env_), -1);
}

TEST_F(SysenvTest, WriteSendsBytesToFdOne) {
  fill_guest(kData + 0x40U, "hello oemu\n");
  EXPECT_EQ(call(OEMU_SYS_WRITE, 1U, kData + 0x40U, 11U), 11);
  EXPECT_EQ(captured(), "hello oemu\n");
}

TEST_F(SysenvTest, WriteSendsFdTwoToTheSameStream) {
  fill_guest(kData, "ab");
  EXPECT_EQ(call(OEMU_SYS_WRITE, 1U, kData, 1U), 1);
  EXPECT_EQ(call(OEMU_SYS_WRITE, 2U, kData + 1U, 1U), 1);
  EXPECT_EQ(captured(), "ab"); /* one stream, in call order */
}

TEST_F(SysenvTest, WriteRejectsOtherFileDescriptors) {
  EXPECT_EQ(call(OEMU_SYS_WRITE, 0U, kData, 1U), -OEMU_EBADF);
  EXPECT_EQ(call(OEMU_SYS_WRITE, 3U, kData, 1U), -OEMU_EBADF);
  EXPECT_EQ(call(OEMU_SYS_WRITE, UINT64_MAX, kData, 1U), -OEMU_EBADF);
  /* The fd check precedes the count check, as on Linux. */
  EXPECT_EQ(call(OEMU_SYS_WRITE, 0U, kData, 0U), -OEMU_EBADF);
}

TEST_F(SysenvTest, WriteOfNothingSucceeds) {
  EXPECT_EQ(call(OEMU_SYS_WRITE, 1U, kData, 0U), 0);
  EXPECT_EQ(captured(), ""); /* unmapped would be fine too: nothing is read */
  EXPECT_EQ(call(OEMU_SYS_WRITE, 1U, kData + kRegion, 0U), 0);
}

TEST_F(SysenvTest, WriteRejectsBadGuestPointers) {
  EXPECT_EQ(call(OEMU_SYS_WRITE, 1U, UINT64_C(0x900000), 4U), -OEMU_EFAULT); /* unmapped */
  EXPECT_EQ(call(OEMU_SYS_WRITE, 1U, UINT64_C(0xFFFFFFFFFFFFFFFC), 8U),
            -OEMU_EFAULT); /* the range wraps */
  EXPECT_EQ(call(OEMU_SYS_WRITE, 1U, kData + kRegion - 2U, 8U), -OEMU_EFAULT); /* runs off */
}

TEST_F(SysenvTest, WriteRefusesABufferSplitAcrossTwoRegions) {
  ASSERT_EQ(oemu_memory_map(&mem_, kData + kRegion, kRegion, OEMU_PERM_READ | OEMU_PERM_WRITE),
            OEMU_OK);
  /* Stitching two regions together would hide a guest bug, so it is an EFAULT. */
  EXPECT_EQ(call(OEMU_SYS_WRITE, 1U, kData + kRegion - 2U, 8U), -OEMU_EFAULT);
  EXPECT_EQ(captured(), "");
}

TEST_F(SysenvTest, WriteRefusesAnUnreadableRegion) {
  ASSERT_EQ(oemu_memory_map(&mem_, kData + 0x8000U, 0x100U, OEMU_PERM_WRITE), OEMU_OK);
  EXPECT_EQ(call(OEMU_SYS_WRITE, 1U, kData + 0x8000U, 4U), -OEMU_EFAULT);
}

TEST_F(SysenvTest, WriteLoopsThroughTheBounceBufferForLongBuffers) {
  /* 600 bytes exercises the chunk loop at 256 + 256 + 88. */
  std::string big;
  big.resize(600U);
  for (size_t i = 0U; i < big.size(); i++) {
    big[i] = static_cast<char>(static_cast<unsigned>(i * 31U + 7U) & 0xFFU);
  }
  fill_guest(kData, big);
  EXPECT_EQ(call(OEMU_SYS_WRITE, 1U, kData, big.size()), 600);
  EXPECT_EQ(captured(), big);
}

TEST_F(SysenvTest, ClockGettimeFillsTwoQwords) {
  /* Poison first: the test must prove the values were written, not assumed. */
  ASSERT_EQ(oemu_memory_write(&mem_, kData, OEMU_MEM_DWORD, UINT64_MAX), OEMU_OK);
  ASSERT_EQ(oemu_memory_write(&mem_, kData + 8U, OEMU_MEM_DWORD, UINT64_MAX), OEMU_OK);
  EXPECT_EQ(call(OEMU_SYS_CLOCK_GETTIME, OEMU_CLOCK_REALTIME, kData), 0);
  uint64_t sec = 0U;
  uint64_t nsec = 0U;
  ASSERT_EQ(oemu_memory_read(&mem_, kData, OEMU_MEM_DWORD, false, &sec), OEMU_OK);
  ASSERT_EQ(oemu_memory_read(&mem_, kData + 8U, OEMU_MEM_DWORD, false, &nsec), OEMU_OK);
  EXPECT_GT(sec, UINT64_C(1700000000)); /* well after 2023, any host clock */
  EXPECT_LT(nsec, UINT64_C(1000000000));

  EXPECT_EQ(call(OEMU_SYS_CLOCK_GETTIME, OEMU_CLOCK_MONOTONIC, kData), 0);
  ASSERT_EQ(oemu_memory_read(&mem_, kData, OEMU_MEM_DWORD, false, &sec), OEMU_OK);
  ASSERT_EQ(oemu_memory_read(&mem_, kData + 8U, OEMU_MEM_DWORD, false, &nsec), OEMU_OK);
  EXPECT_LT(nsec, UINT64_C(1000000000));
  EXPECT_NE(sec, UINT64_MAX);
}

TEST_F(SysenvTest, ClockRejectsUnknownClockIdsBeforeTouchingMemory) {
  ASSERT_EQ(oemu_memory_write(&mem_, kData, OEMU_MEM_DWORD, UINT64_C(0xA5A5A5A5A5A5A5A5)),
            OEMU_OK);
  EXPECT_EQ(call(OEMU_SYS_CLOCK_GETTIME, 2U, kData), -OEMU_EINVAL);
  EXPECT_EQ(call(OEMU_SYS_CLOCK_GETTIME, 7U, kData), -OEMU_EINVAL);
  uint64_t kept = 0U;
  ASSERT_EQ(oemu_memory_read(&mem_, kData, OEMU_MEM_DWORD, false, &kept), OEMU_OK);
  EXPECT_EQ(kept, UINT64_C(0xA5A5A5A5A5A5A5A5));
}

TEST_F(SysenvTest, ClockRejectsBadTimespecPointers) {
  EXPECT_EQ(call(OEMU_SYS_CLOCK_GETTIME, OEMU_CLOCK_MONOTONIC, UINT64_C(0x900000)),
            -OEMU_EFAULT); /* unmapped */
  EXPECT_EQ(call(OEMU_SYS_CLOCK_GETTIME, OEMU_CLOCK_MONOTONIC, kData + kRegion - 4U),
            -OEMU_EFAULT); /* tv_nsec would straddle the end */
  EXPECT_EQ(call(OEMU_SYS_CLOCK_GETTIME, OEMU_CLOCK_MONOTONIC, kData + kRegion),
            -OEMU_EFAULT); /* one past the end */
}

TEST_F(SysenvTest, ClockRejectsReadOnlyTimespec) {
  ASSERT_EQ(oemu_memory_map(&mem_, kData + 0x9000U, 0x40U, OEMU_PERM_READ), OEMU_OK);
  EXPECT_EQ(call(OEMU_SYS_CLOCK_GETTIME, OEMU_CLOCK_MONOTONIC, kData + 0x9000U), -OEMU_EFAULT);
}

}  // namespace
