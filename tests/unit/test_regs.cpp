// Black-box tests for the AArch64 register state module.
//
// The focus is the two architectural rules that the API shape exists to
// enforce: register number 31 means either XZR or SP depending on the
// instruction form, and a 32-bit write zero-extends into the full register.
#include "oemu/regs.h"
#include "oemu/status.h"

#include <cstdint>

#include <gtest/gtest.h>

#include "support/tracking_allocator.h"

namespace {

constexpr std::uint64_t kPattern = 0xDEADBEEFCAFEBABEULL;
constexpr std::uint64_t kLow32 = 0x00000000CAFEBABEULL;
constexpr std::uint64_t kEntryPc = 0x0000000000400078ULL;
constexpr std::uint64_t kInitialSp = 0x0000007FFFFFF000ULL;

// Installing the tracking allocator gives every case a free assertion that the
// module allocates nothing at all -- register state is a fixed-size value type.
class RegsTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_EQ(OEMU_OK, oemu_regs_init(&regs_, kEntryPc, kInitialSp)); }

  void TearDown() override {
    EXPECT_EQ(0u, tracker_.live_blocks()) << "regs module leaked memory";
    EXPECT_EQ(0u, tracker_.alloc_count()) << "regs module must not allocate";
  }

  oemu_test::TrackingAllocator tracker_;
  oemu_regs regs_{};
};

// --- initialisation ----------------------------------------------------------

TEST_F(RegsTest, InitZeroesEveryGeneralRegister) {
  for (unsigned n = 0; n < OEMU_REG_COUNT; ++n) {
    EXPECT_EQ(0u, oemu_regs_read(&regs_, n, OEMU_REG_W64)) << "X" << n;
  }
}

TEST_F(RegsTest, InitSetsPcAndSp) {
  EXPECT_EQ(kEntryPc, oemu_regs_pc(&regs_));
  EXPECT_EQ(kInitialSp, oemu_regs_sp(&regs_));
  EXPECT_EQ(0u, oemu_regs_nzcv(&regs_));
}

TEST_F(RegsTest, InitClearsPreviousState) {
  oemu_regs_write(&regs_, 5, OEMU_REG_W64, kPattern);
  oemu_regs_set_nzcv(&regs_, OEMU_NZCV_MASK);

  ASSERT_EQ(OEMU_OK, oemu_regs_init(&regs_, 0, 0));

  EXPECT_EQ(0u, oemu_regs_read(&regs_, 5, OEMU_REG_W64));
  EXPECT_EQ(0u, oemu_regs_nzcv(&regs_));
  EXPECT_EQ(0u, oemu_regs_pc(&regs_));
  EXPECT_EQ(0u, oemu_regs_sp(&regs_));
}

TEST(RegsInit, RejectsNull) {
  EXPECT_EQ(OEMU_ERR_INVALID_ARG, oemu_regs_init(nullptr, 0, 0));
}

// --- general-purpose registers, 64-bit ---------------------------------------

TEST_F(RegsTest, EveryGeneralRegisterRoundTrips) {
  // Write a distinct value everywhere first, then verify: catches aliasing that
  // a one-register-at-a-time check would miss.
  for (unsigned n = 0; n < OEMU_REG_COUNT; ++n) {
    oemu_regs_write(&regs_, n, OEMU_REG_W64, kPattern ^ n);
  }
  for (unsigned n = 0; n < OEMU_REG_COUNT; ++n) {
    EXPECT_EQ(kPattern ^ n, oemu_regs_read(&regs_, n, OEMU_REG_W64)) << "X" << n;
  }
}

TEST_F(RegsTest, WritingTheLastRegisterDoesNotTouchSpOrPc) {
  // X30 is the final array slot, so an off-by-one would land on sp.
  oemu_regs_write(&regs_, 30, OEMU_REG_W64, UINT64_MAX);

  EXPECT_EQ(UINT64_MAX, oemu_regs_read(&regs_, 30, OEMU_REG_W64));
  EXPECT_EQ(kInitialSp, oemu_regs_sp(&regs_));
  EXPECT_EQ(kEntryPc, oemu_regs_pc(&regs_));
}

// --- 32-bit width semantics --------------------------------------------------

TEST_F(RegsTest, ReadingWFormTruncates) {
  oemu_regs_write(&regs_, 1, OEMU_REG_W64, kPattern);

  EXPECT_EQ(kPattern, oemu_regs_read(&regs_, 1, OEMU_REG_W64));
  EXPECT_EQ(kLow32, oemu_regs_read(&regs_, 1, OEMU_REG_W32));
}

TEST_F(RegsTest, WFormWriteZeroExtends) {
  // The rule most easily got wrong: a W write clears the upper half rather than
  // preserving it.
  oemu_regs_write(&regs_, 2, OEMU_REG_W64, UINT64_MAX);
  oemu_regs_write(&regs_, 2, OEMU_REG_W32, 0x11223344ULL);

  EXPECT_EQ(0x0000000011223344ULL, oemu_regs_read(&regs_, 2, OEMU_REG_W64));
}

TEST_F(RegsTest, WFormWriteDiscardsHighBitsOfTheSource) {
  oemu_regs_write(&regs_, 3, OEMU_REG_W32, kPattern);
  EXPECT_EQ(kLow32, oemu_regs_read(&regs_, 3, OEMU_REG_W64));
}

// --- zero-register form ------------------------------------------------------

TEST_F(RegsTest, ZeroRegisterReadsAsZero) {
  EXPECT_EQ(0u, oemu_regs_read(&regs_, OEMU_REG_ZR, OEMU_REG_W64));
  EXPECT_EQ(0u, oemu_regs_read(&regs_, OEMU_REG_ZR, OEMU_REG_W32));
}

TEST_F(RegsTest, WritingTheZeroRegisterIsDiscarded) {
  oemu_regs_write(&regs_, OEMU_REG_ZR, OEMU_REG_W64, kPattern);
  EXPECT_EQ(0u, oemu_regs_read(&regs_, OEMU_REG_ZR, OEMU_REG_W64));
}

TEST_F(RegsTest, WritingTheZeroRegisterTouchesNothingElse) {
  // XZR and SP share encoding 31. A write through the zero-register form must
  // not reach SP, and must not spill into X30 either.
  oemu_regs_write(&regs_, OEMU_REG_ZR, OEMU_REG_W64, UINT64_MAX);

  EXPECT_EQ(kInitialSp, oemu_regs_sp(&regs_));
  EXPECT_EQ(0u, oemu_regs_read(&regs_, 30, OEMU_REG_W64));
  EXPECT_EQ(kEntryPc, oemu_regs_pc(&regs_));
}

// --- stack-pointer form ------------------------------------------------------

TEST_F(RegsTest, SpFormMapsRegister31ToSp) {
  EXPECT_EQ(kInitialSp, oemu_regs_read_sp_form(&regs_, OEMU_REG_ZR, OEMU_REG_W64));

  oemu_regs_write_sp_form(&regs_, OEMU_REG_ZR, OEMU_REG_W64, 0x1000ULL);
  EXPECT_EQ(0x1000ULL, oemu_regs_sp(&regs_));
  EXPECT_EQ(0x1000ULL, oemu_regs_read_sp_form(&regs_, OEMU_REG_ZR, OEMU_REG_W64));
}

TEST_F(RegsTest, SpFormTruncatesLikeTheZeroRegisterForm) {
  oemu_regs_set_sp(&regs_, kPattern);
  EXPECT_EQ(kLow32, oemu_regs_read_sp_form(&regs_, OEMU_REG_ZR, OEMU_REG_W32));

  oemu_regs_write_sp_form(&regs_, OEMU_REG_ZR, OEMU_REG_W32, UINT64_MAX);
  EXPECT_EQ(0x00000000FFFFFFFFULL, oemu_regs_sp(&regs_));
}

TEST_F(RegsTest, BothFormsAgreeForOrdinaryRegisters) {
  for (unsigned n = 0; n < OEMU_REG_COUNT; ++n) {
    oemu_regs_write_sp_form(&regs_, n, OEMU_REG_W64, kPattern ^ n);
    ASSERT_EQ(oemu_regs_read(&regs_, n, OEMU_REG_W64),
              oemu_regs_read_sp_form(&regs_, n, OEMU_REG_W64))
        << "forms disagree at X" << n;
  }
}

TEST_F(RegsTest, SpAccessorsRoundTrip) {
  oemu_regs_set_sp(&regs_, UINT64_MAX);
  EXPECT_EQ(UINT64_MAX, oemu_regs_sp(&regs_));
}

// --- PC ----------------------------------------------------------------------

TEST_F(RegsTest, AdvancePcStepsOneInstruction) {
  oemu_regs_advance_pc(&regs_);
  EXPECT_EQ(kEntryPc + OEMU_INSN_SIZE, oemu_regs_pc(&regs_));

  oemu_regs_advance_pc(&regs_);
  EXPECT_EQ(kEntryPc + (2 * OEMU_INSN_SIZE), oemu_regs_pc(&regs_));
}

TEST_F(RegsTest, AdvancePcWrapsAtTheTopOfTheAddressSpace) {
  oemu_regs_set_pc(&regs_, UINT64_MAX - 1);
  oemu_regs_advance_pc(&regs_);
  EXPECT_EQ(2u, oemu_regs_pc(&regs_));
}

TEST_F(RegsTest, SetPcRoundTrips) {
  oemu_regs_set_pc(&regs_, kPattern);
  EXPECT_EQ(kPattern, oemu_regs_pc(&regs_));
}

TEST_F(RegsTest, BranchRelMovesForward) {
  oemu_regs_branch_rel(&regs_, 0x40);
  EXPECT_EQ(kEntryPc + 0x40, oemu_regs_pc(&regs_));
}

TEST_F(RegsTest, BranchRelMovesBackward) {
  oemu_regs_branch_rel(&regs_, -0x40);
  EXPECT_EQ(kEntryPc - 0x40, oemu_regs_pc(&regs_));
}

TEST_F(RegsTest, BranchRelZeroIsAnInfiniteLoop) {
  // `b .` -- a real encoding, and a useful check that nothing shifts.
  oemu_regs_branch_rel(&regs_, 0);
  EXPECT_EQ(kEntryPc, oemu_regs_pc(&regs_));
}

TEST_F(RegsTest, BranchRelWrapsBelowZero) {
  oemu_regs_set_pc(&regs_, 4);
  oemu_regs_branch_rel(&regs_, -8);
  EXPECT_EQ(UINT64_MAX - 3, oemu_regs_pc(&regs_));
}

TEST_F(RegsTest, BranchRelHandlesTheExtremeOffsets) {
  oemu_regs_set_pc(&regs_, 0);
  oemu_regs_branch_rel(&regs_, INT64_MIN);
  EXPECT_EQ(0x8000000000000000ULL, oemu_regs_pc(&regs_));

  oemu_regs_set_pc(&regs_, 0);
  oemu_regs_branch_rel(&regs_, INT64_MAX);
  EXPECT_EQ(0x7FFFFFFFFFFFFFFFULL, oemu_regs_pc(&regs_));
}

// --- flags -------------------------------------------------------------------

TEST_F(RegsTest, NzcvRoundTrips) {
  oemu_regs_set_nzcv(&regs_, OEMU_NZCV_N | OEMU_NZCV_C);
  EXPECT_EQ(OEMU_NZCV_N | OEMU_NZCV_C, oemu_regs_nzcv(&regs_));
}

TEST_F(RegsTest, SetNzcvDropsTheReservedBits) {
  oemu_regs_set_nzcv(&regs_, UINT32_MAX);
  EXPECT_EQ(OEMU_NZCV_MASK, oemu_regs_nzcv(&regs_));

  oemu_regs_set_nzcv(&regs_, 0x0FFFFFFFU);
  EXPECT_EQ(0u, oemu_regs_nzcv(&regs_));
}

TEST_F(RegsTest, CondHoldsReadsTheStoredFlags) {
  oemu_regs_set_nzcv(&regs_, OEMU_NZCV_Z);
  EXPECT_TRUE(oemu_regs_cond_holds(&regs_, OEMU_COND_EQ));
  EXPECT_FALSE(oemu_regs_cond_holds(&regs_, OEMU_COND_NE));

  oemu_regs_set_nzcv(&regs_, 0);
  EXPECT_FALSE(oemu_regs_cond_holds(&regs_, OEMU_COND_EQ));
  EXPECT_TRUE(oemu_regs_cond_holds(&regs_, OEMU_COND_NE));
}

TEST_F(RegsTest, AlwaysConditionsHoldRegardlessOfFlags) {
  oemu_regs_set_nzcv(&regs_, 0);
  EXPECT_TRUE(oemu_regs_cond_holds(&regs_, OEMU_COND_AL));
  EXPECT_TRUE(oemu_regs_cond_holds(&regs_, OEMU_COND_NV));

  oemu_regs_set_nzcv(&regs_, OEMU_NZCV_MASK);
  EXPECT_TRUE(oemu_regs_cond_holds(&regs_, OEMU_COND_AL));
  EXPECT_TRUE(oemu_regs_cond_holds(&regs_, OEMU_COND_NV));
}

// --- flag derivation through the public entry point --------------------------

TEST(RegsAlu, AddsWithoutCarry) {
  const oemu_alu_result r = oemu_regs_add_with_carry(2, 3, false, OEMU_REG_W64);
  EXPECT_EQ(5u, r.value);
  EXPECT_EQ(0u, r.nzcv);
}

TEST(RegsAlu, SubtractionIsAddWithComplementAndCarryIn) {
  // CMP x0, x0 with x0 == 5: expect Z and C set, N and V clear.
  const std::uint64_t x = 5;
  const oemu_alu_result r = oemu_regs_add_with_carry(x, ~x, true, OEMU_REG_W64);

  EXPECT_EQ(0u, r.value);
  EXPECT_EQ(OEMU_NZCV_Z | OEMU_NZCV_C, r.nzcv);
}

TEST(RegsAlu, ResultCanBeStoredBackAsFlags) {
  oemu_regs regs{};
  ASSERT_EQ(OEMU_OK, oemu_regs_init(&regs, 0, 0));

  const oemu_alu_result r = oemu_regs_add_with_carry(0, ~UINT64_C(1), true, OEMU_REG_W64);
  oemu_regs_set_nzcv(&regs, r.nzcv);

  // 0 - 1 is negative and borrows, so LT holds and CS does not.
  EXPECT_TRUE(oemu_regs_cond_holds(&regs, OEMU_COND_MI));
  EXPECT_TRUE(oemu_regs_cond_holds(&regs, OEMU_COND_LT));
  EXPECT_FALSE(oemu_regs_cond_holds(&regs, OEMU_COND_CS));
}

}  // namespace
