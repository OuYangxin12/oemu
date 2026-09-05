// Black-box tests for the system-register state and its table-driven accessors.
//
// The identification block pins the values a guest will build its world model
// from, the round-trip tests pin the state plumbing (including the SP_EL0 and
// NZCV fields that live in oemu_regs), and the rejection tests pin the two
// Undefined signals: an unimplemented encoding and an access from too low an
// exception level.
#include "oemu/regs.h"
#include "oemu/sysreg.h"

#include <cstdint>

#include <gtest/gtest.h>

namespace {

class SysregTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(OEMU_OK, oemu_regs_init(&regs_, 0x40080000, 0x41000000));
    oemu_sysregs_init(&sr_, &regs_, OEMU_EL1);
  }

  // Boots a second bank at a given level; several contracts differ per level.
  static void BootAt(oemu_sysregs *sr, oemu_regs *regs, oemu_el el, uint64_t sp) {
    ASSERT_EQ(OEMU_OK, oemu_regs_init(regs, 0x1000, sp));
    oemu_sysregs_init(sr, regs, el);
  }

  oemu_regs regs_{};
  oemu_sysregs sr_{};
};

// --- boot PSTATE ----------------------------------------------------------------

TEST_F(SysregTest, BootPstateAtEl1) {
  const uint64_t expected = OEMU_PSTATE_M_EL1H | OEMU_PSTATE_SPSEL |
                            (OEMU_PSTATE_DAIF_MASK << OEMU_PSTATE_DAIF_SHIFT);
  EXPECT_EQ(expected, sr_.pstate);
  EXPECT_EQ(0u, sr_.pstate & OEMU_PSTATE_IL) << "IL=1 would make boot state illegal";
  EXPECT_EQ(0u, sr_.pstate & OEMU_PSTATE_SS);
}

TEST_F(SysregTest, BootPstateAtEl0UsesTFormAndSpEl0) {
  oemu_sysregs other{};
  BootAt(&other, &regs_, OEMU_EL0, 0x2000);
  EXPECT_EQ(OEMU_PSTATE_M_EL0T, other.pstate & OEMU_PSTATE_M_MASK);
  EXPECT_EQ(0u, other.pstate & OEMU_PSTATE_SPSEL) << "EL0 has no SP_ELx choice";
}

TEST_F(SysregTest, BootPstateAtEl3) {
  oemu_sysregs other{};
  BootAt(&other, &regs_, OEMU_EL3, 0x2000);
  EXPECT_EQ(OEMU_PSTATE_M_EL3H, other.pstate & OEMU_PSTATE_M_MASK);
  EXPECT_EQ(1u, other.pstate & OEMU_PSTATE_SPSEL);
}

// --- the identification block ----------------------------------------------------

TEST_F(SysregTest, IdentificationBlockReadsArchitectedValues) {
  const struct {
    uint32_t sel;
    uint64_t value;
  } rows[] = {
      {OEMU_SYSREG_MIDR_EL1, OEMU_MIDR_EL1},
      {OEMU_SYSREG_MPIDR_EL1, OEMU_MPIDR_EL1},
      {OEMU_SYSREG_REVIDR_EL1, OEMU_REVIDR_EL1},
      {OEMU_SYSREG_ID_AA64PFR0_EL1, OEMU_ID_AA64PFR0_EL1},
      {OEMU_SYSREG_ID_AA64DFR0_EL1, OEMU_ID_AA64DFR0_EL1},
      {OEMU_SYSREG_ID_AA64ISAR0_EL1, OEMU_ID_AA64ISAR0_EL1},
      {OEMU_SYSREG_ID_AA64ISAR1_EL1, OEMU_ID_AA64ISAR1_EL1},
      {OEMU_SYSREG_ID_AA64MMFR0_EL1, OEMU_ID_AA64MMFR0_EL1},
      {OEMU_SYSREG_ID_AA64MMFR1_EL1, OEMU_ID_AA64MMFR1_EL1},
      {OEMU_SYSREG_CTR_EL0, OEMU_CTR_EL0},
      {OEMU_SYSREG_CLIDR_EL1, OEMU_CLIDR_EL1},
      {OEMU_SYSREG_CCSIDR_EL1, OEMU_CCSIDR_EL1},
      {OEMU_SYSREG_DCZID_EL0, OEMU_DCZID_EL0},
  };
  for (const auto &row : rows) {
    uint64_t value = 0;
    EXPECT_EQ(OEMU_OK, oemu_sysreg_read(&sr_, row.sel, &value)) << oemu_sysreg_name(row.sel);
    EXPECT_EQ(row.value, value) << oemu_sysreg_name(row.sel);
  }
}

TEST_F(SysregTest, IdentificationBlockIsReadableFromEl0) {
  oemu_sysregs user{};
  BootAt(&user, &regs_, OEMU_EL0, 0x2000);
  uint64_t midr = 0;
  EXPECT_EQ(OEMU_OK, oemu_sysreg_read(&user, OEMU_SYSREG_MIDR_EL1, &midr));
  EXPECT_EQ(OEMU_MIDR_EL1, midr);
  uint64_t pfr0 = 0;
  EXPECT_EQ(OEMU_OK, oemu_sysreg_read(&user, OEMU_SYSREG_ID_AA64PFR0_EL1, &pfr0));
  EXPECT_EQ(OEMU_ID_AA64PFR0_EL1, pfr0);
}

TEST_F(SysregTest, WriteToIdentificationBlockIsUndefined) {
  uint64_t before = 0;
  ASSERT_EQ(OEMU_OK, oemu_sysreg_read(&sr_, OEMU_SYSREG_MIDR_EL1, &before));
  EXPECT_EQ(OEMU_ERR_UNSUPPORTED, oemu_sysreg_write(&sr_, OEMU_SYSREG_MIDR_EL1, 0));
  uint64_t after = 0;
  EXPECT_EQ(OEMU_OK, oemu_sysreg_read(&sr_, OEMU_SYSREG_MIDR_EL1, &after));
  EXPECT_EQ(before, after) << "a rejected write must not change state";
}

// --- round trips through the table ------------------------------------------------

TEST_F(SysregTest, ControlRegistersRoundTrip) {
  const uint32_t sel_rows[] = {
      OEMU_SYSREG_SCTLR_EL1, OEMU_SYSREG_CPACR_EL1,      OEMU_SYSREG_TTBR0_EL1,
      OEMU_SYSREG_TTBR1_EL1, OEMU_SYSREG_TCR_EL1,        OEMU_SYSREG_MAIR_EL1,
      OEMU_SYSREG_AMAIR_EL1, OEMU_SYSREG_CONTEXTIDR_EL1, OEMU_SYSREG_TPIDR_EL1,
      OEMU_SYSREG_TPIDR_EL0, OEMU_SYSREG_VBAR_EL1,       OEMU_SYSREG_ESR_EL1,
      OEMU_SYSREG_FAR_EL1,   OEMU_SYSREG_ELR_EL1,        OEMU_SYSREG_SPSR_EL1,
  };
  for (const uint32_t sel : sel_rows) {
    const uint64_t probe = 0xA5A5A5A5A5A5A5A5ULL ^ sel;
    EXPECT_EQ(OEMU_OK, oemu_sysreg_write(&sr_, sel, probe)) << oemu_sysreg_name(sel);
    uint64_t value = 0;
    EXPECT_EQ(OEMU_OK, oemu_sysreg_read(&sr_, sel, &value)) << oemu_sysreg_name(sel);
    EXPECT_EQ(probe, value) << oemu_sysreg_name(sel);
  }
}

TEST_F(SysregTest, El3BanksRoundTripOnlyFromEl3) {
  uint64_t value = 0;
  EXPECT_EQ(OEMU_ERR_UNSUPPORTED, oemu_sysreg_read(&sr_, OEMU_SYSREG_VBAR_EL3, &value))
      << "EL3 state from EL1 is Undefined";

  oemu_sysregs monitor{};
  BootAt(&monitor, &regs_, OEMU_EL3, 0x2000);
  EXPECT_EQ(OEMU_OK, oemu_sysreg_write(&monitor, OEMU_SYSREG_VBAR_EL3, 0x8000));
  EXPECT_EQ(OEMU_OK, oemu_sysreg_read(&monitor, OEMU_SYSREG_VBAR_EL3, &value));
  EXPECT_EQ(0x8000u, value);
}

TEST_F(SysregTest, ExceptionBanksStartZeroed) {
  uint64_t vbar = 0xFFFFFFFFFFFFFFFFULL;
  ASSERT_EQ(OEMU_OK, oemu_sysreg_read(&sr_, OEMU_SYSREG_VBAR_EL1, &vbar));
  EXPECT_EQ(0u, vbar);
}

TEST_F(SysregTest, SpEl0RoundTripsThroughTheRegisterFile) {
  EXPECT_EQ(OEMU_OK, oemu_sysreg_write(&sr_, OEMU_SYSREG_SP_EL0, 0x7FFF0000));
  EXPECT_EQ(0x7FFF0000u, oemu_regs_sp(&regs_)) << "SP_EL0 must land in oemu_regs.sp";
  oemu_regs_set_sp(&regs_, 0x1234);
  uint64_t value = 0;
  EXPECT_EQ(OEMU_OK, oemu_sysreg_read(&sr_, OEMU_SYSREG_SP_EL0, &value));
  EXPECT_EQ(0x1234u, value);
}

TEST_F(SysregTest, NzcvRoundTripsThroughTheRegisterFileAndMasks) {
  // Junk above bit 31 and below bit 28 must be dropped by the register file;
  // the probe keeps only its flag bits after the uint32_t truncation.
  EXPECT_EQ(OEMU_OK, oemu_sysreg_write(&sr_, OEMU_SYSREG_NZCV, 0xDEADBEEFF0000000ULL));
  EXPECT_EQ(OEMU_NZCV_N | OEMU_NZCV_Z | OEMU_NZCV_C | OEMU_NZCV_V, oemu_regs_nzcv(&regs_));
  uint64_t value = 0;
  EXPECT_EQ(OEMU_OK, oemu_sysreg_read(&sr_, OEMU_SYSREG_NZCV, &value));
  EXPECT_EQ(0xF0000000u, value);
}

TEST_F(SysregTest, SpselAndDaifRoundTrip) {
  EXPECT_EQ(OEMU_OK, oemu_sysreg_write(&sr_, OEMU_SYSREG_SPSEL, 0));
  uint64_t value = 1;
  EXPECT_EQ(OEMU_OK, oemu_sysreg_read(&sr_, OEMU_SYSREG_SPSEL, &value));
  EXPECT_EQ(0u, value);
  EXPECT_EQ(OEMU_OK, oemu_sysreg_write(&sr_, OEMU_SYSREG_SPSEL, 1));
  EXPECT_EQ(OEMU_OK, oemu_sysreg_read(&sr_, OEMU_SYSREG_SPSEL, &value));
  EXPECT_EQ(1u, value);

  EXPECT_EQ(OEMU_OK, oemu_sysreg_write(&sr_, OEMU_SYSREG_DAIF, 0b1010));
  value = 0;
  EXPECT_EQ(OEMU_OK, oemu_sysreg_read(&sr_, OEMU_SYSREG_DAIF, &value));
  EXPECT_EQ(0b1010u, value);

  // Junk above the 4-bit field must not leak into neighbouring PSTATE bits:
  // mark IL/SS first, then a max-width DAIF write, then verify both survive.
  sr_.pstate |= OEMU_PSTATE_IL | OEMU_PSTATE_SS;
  EXPECT_EQ(OEMU_OK, oemu_sysreg_write(&sr_, OEMU_SYSREG_DAIF, 0xFFFFFFFFFFFFFFFFULL));
  EXPECT_EQ(OEMU_PSTATE_IL | OEMU_PSTATE_SS, sr_.pstate & (OEMU_PSTATE_IL | OEMU_PSTATE_SS))
      << "DAIF write clobbered IL/SS";
  EXPECT_EQ(OEMU_OK, oemu_sysreg_read(&sr_, OEMU_SYSREG_DAIF, &value));
  EXPECT_EQ(0xFu, value);
}

TEST_F(SysregTest, CurrentElReportsTheBootLevel) {
  uint64_t value = 0;
  EXPECT_EQ(OEMU_OK, oemu_sysreg_read(&sr_, OEMU_SYSREG_CURRENT_EL, &value));
  EXPECT_EQ(4u, value);

  oemu_sysregs user{};
  BootAt(&user, &regs_, OEMU_EL0, 0x2000);
  EXPECT_EQ(OEMU_ERR_UNSUPPORTED, oemu_sysreg_read(&user, OEMU_SYSREG_CURRENT_EL, &value))
      << "CurrentEL is EL1-and-above";

  oemu_sysregs monitor{};
  BootAt(&monitor, &regs_, OEMU_EL3, 0x2000);
  EXPECT_EQ(OEMU_OK, oemu_sysreg_read(&monitor, OEMU_SYSREG_CURRENT_EL, &value));
  EXPECT_EQ(12u, value);
}

// --- the two Undefined signals -----------------------------------------------------

TEST_F(SysregTest, UnimplementedEncodingsAreUnsupported) {
  uint64_t value = 0;
  // 0x1E87 is the phantom "TPIDRUR_EL0" encoding the EL0 whitelist once used;
  // no such register exists, so pin it as unknown here.
  EXPECT_EQ(OEMU_ERR_UNSUPPORTED, oemu_sysreg_read(&sr_, 0x1E87, &value));
  EXPECT_EQ(OEMU_ERR_UNSUPPORTED, oemu_sysreg_write(&sr_, 0x1E87, 0));
  EXPECT_EQ(OEMU_ERR_UNSUPPORTED, oemu_sysreg_read(&sr_, 0x3FFF, &value));
  EXPECT_STREQ("unknown", oemu_sysreg_name(0x1E87));
}

TEST_F(SysregTest, CsSElrIsWriteIgnored) {
  EXPECT_EQ(OEMU_OK, oemu_sysreg_write(&sr_, OEMU_SYSREG_CSSELR_EL1, 5));
  uint64_t value = 0;
  EXPECT_EQ(OEMU_OK, oemu_sysreg_read(&sr_, OEMU_SYSREG_CSSELR_EL1, &value));
  EXPECT_EQ(0u, value);
}

TEST_F(SysregTest, ReadOnlyRegistersRejectWrites) {
  const uint32_t sel_rows[] = {
      OEMU_SYSREG_TPIDRRO_EL0, OEMU_SYSREG_CTR_EL0,    OEMU_SYSREG_DCZID_EL0,
      OEMU_SYSREG_CLIDR_EL1,   OEMU_SYSREG_CCSIDR_EL1, OEMU_SYSREG_CURRENT_EL,
  };
  for (const uint32_t sel : sel_rows) {
    EXPECT_EQ(OEMU_ERR_UNSUPPORTED, oemu_sysreg_write(&sr_, sel, 1)) << oemu_sysreg_name(sel);
  }
}

// --- exception-level gating ---------------------------------------------------------

TEST_F(SysregTest, El0CannotReachEl1State) {
  oemu_sysregs user{};
  BootAt(&user, &regs_, OEMU_EL0, 0x2000);
  uint64_t value = 0;
  const uint32_t el1_rows[] = {
      OEMU_SYSREG_VBAR_EL1,  OEMU_SYSREG_SCTLR_EL1, OEMU_SYSREG_SPSEL,      OEMU_SYSREG_DAIF,
      OEMU_SYSREG_TPIDR_EL1, OEMU_SYSREG_CLIDR_EL1, OEMU_SYSREG_CURRENT_EL, OEMU_SYSREG_SP_EL1,
  };
  for (const uint32_t sel : el1_rows) {
    EXPECT_EQ(OEMU_ERR_UNSUPPORTED, oemu_sysreg_read(&user, sel, &value))
        << oemu_sysreg_name(sel);
    EXPECT_EQ(OEMU_ERR_UNSUPPORTED, oemu_sysreg_write(&user, sel, 0)) << oemu_sysreg_name(sel);
  }
  // ... while the EL0-visible subset still works from EL0.
  EXPECT_EQ(OEMU_OK, oemu_sysreg_read(&user, OEMU_SYSREG_CTR_EL0, &value));
  EXPECT_EQ(OEMU_OK, oemu_sysreg_read(&user, OEMU_SYSREG_SP_EL0, &value));
  EXPECT_EQ(OEMU_OK, oemu_sysreg_read(&user, OEMU_SYSREG_NZCV, &value));
  EXPECT_EQ(OEMU_OK, oemu_sysreg_read(&user, OEMU_SYSREG_TPIDR_EL0, &value));
}

TEST_F(SysregTest, SpEl1RequiresEl2OrAbove) {
  // SP_ELx is encoded in the op1 bank of the level above it, so SP_EL1 is
  // reachable from EL2 and EL3 only. An EL1 guest reads its own stack
  // pointer through SP plus SPSel instead.
  uint64_t value = 0;
  EXPECT_EQ(OEMU_ERR_UNSUPPORTED, oemu_sysreg_read(&sr_, OEMU_SYSREG_SP_EL1, &value));

  oemu_sysregs monitor{};
  BootAt(&monitor, &regs_, OEMU_EL3, 0x2000);
  EXPECT_EQ(OEMU_OK, oemu_sysreg_write(&monitor, OEMU_SYSREG_SP_EL1, 0x6000));
  EXPECT_EQ(OEMU_OK, oemu_sysreg_read(&monitor, OEMU_SYSREG_SP_EL1, &value));
  EXPECT_EQ(0x6000u, value);
}

TEST_F(SysregTest, El3CanReachEl1State) {
  oemu_sysregs monitor{};
  BootAt(&monitor, &regs_, OEMU_EL3, 0x2000);
  uint64_t value = 0;
  EXPECT_EQ(OEMU_OK, oemu_sysreg_read(&monitor, OEMU_SYSREG_VBAR_EL1, &value));
  EXPECT_EQ(OEMU_OK, oemu_sysreg_write(&monitor, OEMU_SYSREG_TTBR0_EL1, 0x42));
  EXPECT_EQ(OEMU_OK, oemu_sysreg_read(&monitor, OEMU_SYSREG_TTBR0_EL1, &value));
  EXPECT_EQ(0x42u, value);
}

TEST_F(SysregTest, NameIsNeverNull) {
  EXPECT_STREQ("NZCV", oemu_sysreg_name(OEMU_SYSREG_NZCV));
  EXPECT_STREQ("VBAR_EL1", oemu_sysreg_name(OEMU_SYSREG_VBAR_EL1));
  EXPECT_NE(nullptr, oemu_sysreg_name(0));
  EXPECT_NE(nullptr, oemu_sysreg_name(0x3FFF));
}

}  // namespace
