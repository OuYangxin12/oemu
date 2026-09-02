// Death tests for the register module's fatal contracts.
//
// A register number comes from a 5-bit instruction field and a width from the
// `sf` bit, so an invalid value can only mean the caller is broken -- there is
// no sensible status to return, and continuing would corrupt guest state
// silently. Those contracts therefore abort, and abort is only observable from a
// forked child, which is why they live in their own file.
//
// The "threadsafe" style re-execs instead of forking mid-state; the default
// "fast" style can report spurious leaks from the aborted child under ASan.
#include "oemu/regs.h"
#include "oemu/status.h"

#include <cstdint>

#include <gtest/gtest.h>

namespace {

class RegsDeathTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ::testing::FLAGS_gtest_death_test_style = "threadsafe";
    ASSERT_EQ(OEMU_OK, oemu_regs_init(&regs_, 0x1000, 0x2000));
  }

  oemu_regs regs_{};
};

// --- register numbers --------------------------------------------------------

TEST_F(RegsDeathTest, ReadRejectsRegisterNumberAbove31) {
  EXPECT_DEATH({ (void)oemu_regs_read(&regs_, 32, OEMU_REG_W64); }, "register number above 31");
}

TEST_F(RegsDeathTest, WriteRejectsRegisterNumberAbove31) {
  EXPECT_DEATH({ oemu_regs_write(&regs_, 32, OEMU_REG_W64, 0); }, "register number above 31");
}

TEST_F(RegsDeathTest, SpFormRejectsRegisterNumberAbove31) {
  EXPECT_DEATH(
      { (void)oemu_regs_read_sp_form(&regs_, 99, OEMU_REG_W64); }, "register number above 31");
  EXPECT_DEATH(
      { oemu_regs_write_sp_form(&regs_, 99, OEMU_REG_W64, 0); }, "register number above 31");
}

TEST_F(RegsDeathTest, ReadRejectsAWildRegisterNumber) {
  EXPECT_DEATH(
      { (void)oemu_regs_read(&regs_, 0xFFFFFFFFU, OEMU_REG_W64); }, "register number above 31");
}

TEST_F(RegsDeathTest, Register31IsAcceptedNotRejected) {
  // The boundary: 31 is a valid encoding, just not an array index.
  EXPECT_EQ(0u, oemu_regs_read(&regs_, 31, OEMU_REG_W64));
  EXPECT_EQ(0x2000u, oemu_regs_read_sp_form(&regs_, 31, OEMU_REG_W64));
}

// --- widths ------------------------------------------------------------------

TEST_F(RegsDeathTest, ReadRejectsAnInvalidWidth) {
  EXPECT_DEATH(
      { (void)oemu_regs_read(&regs_, 0, static_cast<oemu_reg_width>(16)); }, "operand width");
}

TEST_F(RegsDeathTest, WriteRejectsAnInvalidWidth) {
  EXPECT_DEATH(
      { oemu_regs_write(&regs_, 0, static_cast<oemu_reg_width>(0), 0); }, "operand width");
}

TEST_F(RegsDeathTest, AddWithCarryRejectsAnInvalidWidth) {
  EXPECT_DEATH(
      { (void)oemu_regs_add_with_carry(1, 1, false, static_cast<oemu_reg_width>(8)); },
      "operand width");
}

// --- NULL state --------------------------------------------------------------

TEST_F(RegsDeathTest, AccessorsRejectNullState) {
  EXPECT_DEATH({ (void)oemu_regs_read(nullptr, 0, OEMU_REG_W64); }, "NULL oemu_regs");
  EXPECT_DEATH({ oemu_regs_write(nullptr, 0, OEMU_REG_W64, 0); }, "NULL oemu_regs");
}

TEST_F(RegsDeathTest, PcHelpersRejectNullState) {
  EXPECT_DEATH({ (void)oemu_regs_pc(nullptr); }, "NULL oemu_regs");
  EXPECT_DEATH({ oemu_regs_advance_pc(nullptr); }, "NULL oemu_regs");
  EXPECT_DEATH({ oemu_regs_branch_rel(nullptr, 4); }, "NULL oemu_regs");
  EXPECT_DEATH({ oemu_regs_set_pc(nullptr, 0); }, "NULL oemu_regs");
}

TEST_F(RegsDeathTest, SpHelpersRejectNullState) {
  EXPECT_DEATH({ (void)oemu_regs_sp(nullptr); }, "NULL oemu_regs");
  EXPECT_DEATH({ oemu_regs_set_sp(nullptr, 0); }, "NULL oemu_regs");
}

TEST_F(RegsDeathTest, FlagHelpersRejectNullState) {
  EXPECT_DEATH({ (void)oemu_regs_nzcv(nullptr); }, "NULL oemu_regs");
  EXPECT_DEATH({ oemu_regs_set_nzcv(nullptr, 0); }, "NULL oemu_regs");
  EXPECT_DEATH({ (void)oemu_regs_cond_holds(nullptr, OEMU_COND_EQ); }, "NULL oemu_regs");
}

// A NULL at init is recoverable -- the caller is at a point where it can react,
// so that one path returns a status instead of aborting.
TEST_F(RegsDeathTest, InitReportsNullInsteadOfAborting) {
  EXPECT_EQ(OEMU_ERR_INVALID_ARG, oemu_regs_init(nullptr, 0, 0));
}

}  // namespace
