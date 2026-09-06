// Death tests for the exception module's fatal contracts.
//
// NULL state and an impossible delivery target are caller bugs -- there is no
// sensible status to return -- so they abort, and abort is only observable
// from a forked child, which is why they live in their own file.
//
// The "threadsafe" style re-execs instead of forking mid-state; the default
// "fast" style can report spurious leaks from the aborted child under ASan.
#include "oemu/exc.h"
#include "oemu/regs.h"
#include "oemu/sysreg.h"

#include <cstdint>

#include <gtest/gtest.h>

namespace {

class ExcDeathTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    ASSERT_EQ(OEMU_OK, oemu_regs_init(&regs_, 0x1000, 0x2000));
    oemu_sysregs_init(&sr_, &regs_, OEMU_EL1);
  }

  oemu_regs regs_{};
  oemu_sysregs sr_{};
};

TEST_F(ExcDeathTest, TakeRejectsNullRegs) {
  EXPECT_DEATH(
      { oemu_exc_take(NULL, &sr_, OEMU_EXC_KIND_SYNC, OEMU_EL1, 0, 0, false); },
      "NULL oemu_regs");
}

TEST_F(ExcDeathTest, TakeRejectsNullSysregs) {
  EXPECT_DEATH(
      { oemu_exc_take(&regs_, NULL, OEMU_EXC_KIND_SYNC, OEMU_EL1, 0, 0, false); },
      "NULL oemu_sysregs");
}

TEST_F(ExcDeathTest, TakeRejectsDeliveryToEl0) {
  // Exceptions architecturally never target EL0 in the AArch64 model.
  EXPECT_DEATH(
      { oemu_exc_take(&regs_, &sr_, OEMU_EXC_KIND_SYNC, OEMU_EL0, 0, 0, false); },
      "target must be EL1 or EL3");
}

TEST_F(ExcDeathTest, TakeRejectsDeliveryToEl2) {
  // oemu implements no EL2, so no delivery path can name it.
  EXPECT_DEATH(
      { oemu_exc_take(&regs_, &sr_, OEMU_EXC_KIND_SYNC, OEMU_EL2, 0, 0, false); },
      "target must be EL1 or EL3");
}

TEST_F(ExcDeathTest, EretRejectsNullRegs) {
  EXPECT_DEATH({ oemu_exc_eret(NULL, &sr_); }, "NULL oemu_regs");
}

TEST_F(ExcDeathTest, EretRejectsNullSysregs) {
  EXPECT_DEATH({ oemu_exc_eret(&regs_, NULL); }, "NULL oemu_sysregs");
}

TEST_F(ExcDeathTest, SwitchSpRejectsNullSysregs) {
  EXPECT_DEATH({ oemu_sysregs_switch_sp(NULL, 0); }, "NULL oemu_sysregs");
}

TEST_F(ExcDeathTest, SwitchSpRejectsAnUnpairedSysregs) {
  // A bank struct whose regs pointer never got paired by init cannot keep the
  // mirroring invariant, so the switch must refuse rather than dereference.
  oemu_sysregs orphan{};
  EXPECT_DEATH(
      { oemu_sysregs_switch_sp(&orphan, 0); }, "NULL oemu_regs in oemu_sysregs_switch_sp");
}

}  // namespace
