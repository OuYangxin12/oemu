// Death tests for the system-register module's fatal contracts.
//
// A NULL state pointer or an EL2 boot are caller bugs the API cannot report:
// there is no sensible status to return, and continuing would corrupt guest
// state silently. They therefore abort, and abort is only observable from a
// forked child, which is why they live in their own file.
//
// The "threadsafe" style re-execs instead of forking mid-state; the default
// "fast" style can report spurious leaks from the aborted child under ASan.
#include "oemu/regs.h"
#include "oemu/sysreg.h"

#include <cstdint>

#include <gtest/gtest.h>

namespace {

class SysregDeathTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    ASSERT_EQ(OEMU_OK, oemu_regs_init(&regs_, 0x1000, 0x2000));
    oemu_sysregs_init(&sr_, &regs_, OEMU_EL1);
  }

  oemu_regs regs_{};
  oemu_sysregs sr_{};
};

TEST_F(SysregDeathTest, InitRejectsEl2Boot) {
  // oemu implements no EL2 (roadmap D2); booting there is a programming error.
  EXPECT_DEATH({ oemu_sysregs_init(&sr_, &regs_, OEMU_EL2); }, "no EL2");
}

TEST_F(SysregDeathTest, InitRejectsNullSysregs) {
  EXPECT_DEATH({ oemu_sysregs_init(nullptr, &regs_, OEMU_EL1); }, "NULL oemu_sysregs");
}

TEST_F(SysregDeathTest, InitRejectsNullRegs) {
  EXPECT_DEATH({ oemu_sysregs_init(&sr_, nullptr, OEMU_EL1); }, "NULL oemu_regs");
}

TEST_F(SysregDeathTest, ReadRejectsNullState) {
  uint64_t value = 0;
  EXPECT_DEATH(
      {
        // GCC's warn_unused_result ignores a bare (void) cast; consume the
        // status into a discarded local instead.
        oemu_status unused = oemu_sysreg_read(nullptr, OEMU_SYSREG_NZCV, &value);
        (void)unused;
      },
      "NULL oemu_sysregs");
}

TEST_F(SysregDeathTest, ReadRejectsNullOut) {
  EXPECT_DEATH(
      {
        oemu_status unused = oemu_sysreg_read(&sr_, OEMU_SYSREG_NZCV, nullptr);
        (void)unused;
      },
      "NULL out for oemu_sysreg_read");
}

TEST_F(SysregDeathTest, WriteRejectsNullState) {
  EXPECT_DEATH(
      {
        oemu_status unused = oemu_sysreg_write(nullptr, OEMU_SYSREG_NZCV, 0);
        (void)unused;
      },
      "NULL oemu_sysregs");
}

}  // namespace
