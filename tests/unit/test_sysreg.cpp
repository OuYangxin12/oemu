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
  // Booting at EL1h leaves SP_EL0 inactive: a write lands in its bank and
  // only becomes the live SP once SPSel selects it (the bank model of
  // oemu/sysreg.h).
  EXPECT_EQ(OEMU_OK, oemu_sysreg_write(&sr_, OEMU_SYSREG_SP_EL0, 0x7FFF0000U));
  EXPECT_EQ(0x41000000U, oemu_regs_sp(&regs_)) << "the active SP_EL1 must not move";
  oemu_regs_set_sp(&regs_, 0x1234);  // the kernel stack moves underneath
  uint64_t value = 0;
  EXPECT_EQ(OEMU_OK, oemu_sysreg_read(&sr_, OEMU_SYSREG_SP_EL0, &value));
  EXPECT_EQ(0x7FFF0000U, value) << "SP_EL0 keeps its banked value";

  EXPECT_EQ(OEMU_OK, oemu_sysreg_write(&sr_, OEMU_SYSREG_SPSEL, 0));
  EXPECT_EQ(0x7FFF0000U, oemu_regs_sp(&regs_)) << "selecting SP_EL0 adopts its bank";

  oemu_regs_set_sp(&regs_, 0x1234);
  EXPECT_EQ(OEMU_OK, oemu_sysreg_read(&sr_, OEMU_SYSREG_SP_EL0, &value));
  EXPECT_EQ(0x1234U, value) << "an active SP_EL0 reads back through regs";
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

  /* MRS/MSR DAIF use the PSTATE positions (bits [9:6]), like NZCV at [31:28]. */
  EXPECT_EQ(OEMU_OK,
            oemu_sysreg_write(&sr_, OEMU_SYSREG_DAIF, 0b1010U << OEMU_PSTATE_DAIF_SHIFT));
  value = 0;
  EXPECT_EQ(OEMU_OK, oemu_sysreg_read(&sr_, OEMU_SYSREG_DAIF, &value));
  EXPECT_EQ(0b1010U << OEMU_PSTATE_DAIF_SHIFT, value);

  // Junk above the 4-bit field must not leak into neighbouring PSTATE bits:
  // mark IL/SS first, then a max-width DAIF write, then verify both survive.
  sr_.pstate |= OEMU_PSTATE_IL | OEMU_PSTATE_SS;
  EXPECT_EQ(OEMU_OK, oemu_sysreg_write(&sr_, OEMU_SYSREG_DAIF, 0xFFFFFFFFFFFFFFFFULL));
  EXPECT_EQ(OEMU_PSTATE_IL | OEMU_PSTATE_SS, sr_.pstate & (OEMU_PSTATE_IL | OEMU_PSTATE_SS))
      << "DAIF write clobbered IL/SS";
  EXPECT_EQ(OEMU_OK, oemu_sysreg_read(&sr_, OEMU_SYSREG_DAIF, &value));
  EXPECT_EQ(OEMU_PSTATE_DAIF_MASK << OEMU_PSTATE_DAIF_SHIFT, value);
}

TEST_F(SysregTest, MsrDaifRegisterFormMasksTheInterruptItNames) {
  /* The value the guest hands MSR DAIF, Xt is the mask at the PSTATE positions;
   * Linux's IRQ entry writes 0xc0 (I|F) there. */
  ASSERT_EQ(OEMU_OK, oemu_sysreg_write(&sr_, OEMU_SYSREG_DAIF, 0xC0U));
  EXPECT_EQ(0xC0U, sr_.pstate & 0xC0U) << "I and F must be masked";
  EXPECT_EQ(0U, sr_.pstate & (0xCU << OEMU_PSTATE_DAIF_SHIFT)) << "D and A must be clear";
  EXPECT_EQ(3U, oemu_pstate_daif(sr_.pstate));
  uint64_t value = 0;
  ASSERT_EQ(OEMU_OK, oemu_sysreg_read(&sr_, OEMU_SYSREG_DAIF, &value));
  EXPECT_EQ(0xC0U, value);
}

TEST_F(SysregTest, TimerControlReportsIstatFromTheLiveComparator) {
  /* CNTV_CTL bit 2 is read-only ISTAT: set while the timer is enabled, unmasked
   * and expired. Linux's arch timer handler gates its re-arm on this bit, so a
   * hardwired zero made it answer IRQ_NONE forever and the PPI stayed pending. */
  sr_.cntvct = 1000U;
  sr_.cntvoff_el1 = 0U;
  sr_.cntv_cval_el1 = 2000U;
  ASSERT_EQ(OEMU_OK, oemu_sysreg_write(&sr_, OEMU_SYSREG_CNTV_CTL_EL0, 0x1U));
  uint64_t value = 0;
  ASSERT_EQ(OEMU_OK, oemu_sysreg_read(&sr_, OEMU_SYSREG_CNTV_CTL_EL0, &value));
  EXPECT_EQ(0x1U, value) << "not expired yet: ISTAT clear";
  sr_.cntvct = 2000U;
  ASSERT_EQ(OEMU_OK, oemu_sysreg_read(&sr_, OEMU_SYSREG_CNTV_CTL_EL0, &value));
  EXPECT_EQ(0x5U, value) << "expired: ISTAT set (bit 2)";
  /* ISTAT is read-only: writing it back must not stick, and masking the
   * interrupt clears the reported status without touching the enable bit. */
  ASSERT_EQ(OEMU_OK, oemu_sysreg_write(&sr_, OEMU_SYSREG_CNTV_CTL_EL0, 0x7U));
  ASSERT_EQ(OEMU_OK, oemu_sysreg_read(&sr_, OEMU_SYSREG_CNTV_CTL_EL0, &value));
  EXPECT_EQ(0x3U, value) << "enable+imask stored; ISTAT recomputed to 0";
}

TEST_F(SysregTest, TimerTvalIsADeltaOnTheLiveCounter) {
  /* Linux's clockevent reprograms the timer through TVAL on every tick; the
   * hardware turns the delta into an absolute comparator against the counter. */
  sr_.cntvct = 5000U;
  sr_.cntvoff_el1 = 1000U;
  ASSERT_EQ(OEMU_OK, oemu_sysreg_write(&sr_, OEMU_SYSREG_CNTV_TVAL_EL0, 250U));
  EXPECT_EQ(4250U, sr_.cntv_cval_el1) << "cval = (cntvct - cntvoff) + tval";
  uint64_t value = 0;
  ASSERT_EQ(OEMU_OK, oemu_sysreg_read(&sr_, OEMU_SYSREG_CNTV_TVAL_EL0, &value));
  EXPECT_EQ(250U, value);
  sr_.cntvct = 4000U;
  ASSERT_EQ(OEMU_OK, oemu_sysreg_write(&sr_, OEMU_SYSREG_CNTP_TVAL_EL1, 100U));
  EXPECT_EQ(4100U, sr_.cntp_cval_el1) << "the physical bank has no virtual offset";
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

// --- the banked stack pointers -------------------------------------------------

TEST_F(SysregTest, MsrSpselSwitchesTheActiveBank) {
  // MSR SPSel really switches stacks (that is its whole purpose on the kernel
  // entry path): the newly selected bank becomes the interpreter's SP.
  EXPECT_EQ(0x41000000U, regs_.sp);
  EXPECT_EQ(0x41000000U, sr_.sp_el[OEMU_EL1]) << "boot seeds the active bank";

  EXPECT_EQ(OEMU_OK, oemu_sysreg_write(&sr_, OEMU_SYSREG_SPSEL, 0));
  EXPECT_EQ(0U, regs_.sp) << "SP_EL0 was never written: bank switch reads its stored 0";
  EXPECT_EQ(0x41000000U, sr_.sp_el[OEMU_EL1]) << "the kernel stack is preserved in its bank";

  EXPECT_EQ(OEMU_OK, oemu_sysreg_write(&sr_, OEMU_SYSREG_SPSEL, 1));
  EXPECT_EQ(0x41000000U, regs_.sp);
}

TEST_F(SysregTest, WritingSpEl0WhileInactiveStaysInItsBank) {
  // KVM-style sequence: prepare a user stack from EL1, then let an ERET to
  // EL0t pick it up.
  EXPECT_EQ(OEMU_OK, oemu_sysreg_write(&sr_, OEMU_SYSREG_SP_EL0, 0xABCDU));
  uint64_t value = 0;
  EXPECT_EQ(OEMU_OK, oemu_sysreg_read(&sr_, OEMU_SYSREG_SP_EL0, &value));
  EXPECT_EQ(0xABCDU, value);
  EXPECT_EQ(0x41000000U, regs_.sp) << "the active SP_EL1 must not move";

  EXPECT_EQ(OEMU_OK, oemu_sysreg_write(&sr_, OEMU_SYSREG_SPSEL, 0));
  EXPECT_EQ(0xABCDU, regs_.sp) << "switching to SP_EL0 adopts the prepared value";
}

TEST_F(SysregTest, SpEl1IsReachableFromEl3IntoItsBank) {
  oemu_sysregs monitor{};
  BootAt(&monitor, &regs_, OEMU_EL3, 0x2000);

  EXPECT_EQ(OEMU_OK, oemu_sysreg_write(&monitor, OEMU_SYSREG_SP_EL1, 0x9999U));
  EXPECT_EQ(0x9999U, monitor.sp_el[OEMU_EL1]);
  EXPECT_EQ(0x2000U, regs_.sp) << "an inactive bank write must not move the live SP";

  uint64_t value = 0;
  EXPECT_EQ(OEMU_OK, oemu_sysreg_read(&monitor, OEMU_SYSREG_SP_EL1, &value));
  EXPECT_EQ(0x9999U, value);
}

TEST_F(SysregTest, SpEl1IsInaccessibleFromEl1) {
  // The SP_EL1 encoding lives in the op1 bank of the level above, so an EL1
  // guest cannot touch it -- the executor's Undefined signal, same as any
  // too-low-EL access.
  uint64_t value = 0;
  EXPECT_EQ(OEMU_ERR_UNSUPPORTED, oemu_sysreg_read(&sr_, OEMU_SYSREG_SP_EL1, &value));
  EXPECT_EQ(OEMU_ERR_UNSUPPORTED, oemu_sysreg_write(&sr_, OEMU_SYSREG_SP_EL1, 0));
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
      OEMU_SYSREG_CTR_EL0,    OEMU_SYSREG_DCZID_EL0,  OEMU_SYSREG_CLIDR_EL1,
      OEMU_SYSREG_CCSIDR_EL1, OEMU_SYSREG_CURRENT_EL,
  };
  for (const uint32_t sel : sel_rows) {
    EXPECT_EQ(OEMU_ERR_UNSUPPORTED, oemu_sysreg_write(&sr_, sel, 1)) << oemu_sysreg_name(sel);
  }
}

// TPIDRRO_EL0 is "EL0 read-only", not read-only: EL1 owns it and the kernel's
// __switch_to clears it on every context switch, so a write from EL1 must land.
TEST_F(SysregTest, TpidrroEl0IsWritableFromEl1) {
  EXPECT_EQ(OEMU_OK, oemu_sysreg_write(&sr_, OEMU_SYSREG_TPIDRRO_EL0, 0x1234));
  uint64_t value = 0;
  EXPECT_EQ(OEMU_OK, oemu_sysreg_read(&sr_, OEMU_SYSREG_TPIDRRO_EL0, &value));
  EXPECT_EQ(0x1234u, value);
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

TEST_F(SysregTest, TtbrWritesLandWholeAndDoNotDisturbEachOther) {
  /* The M5 boot investigation currently rests on one contradiction: the guest is
   * caught running on swapper_pg_dir at every interrupt delivery although the log
   * position implies init_pg_dir is live. A TTBR write that did not land, or that
   * landed with bits eaten by a mask, would explain that outright -- so the
   * round-trip is pinned here, at the three addresses the boot actually uses and
   * at an ASID-tagged value whose high bits a masking bug would swallow. */
  const uint64_t values[] = {0x4022c000ULL, 0x40341000ULL, 0x4022e000ULL,
                             0x4022e000ULL | (0x123ULL << 48)};
  for (unsigned i = 0U; i < (sizeof(values) / sizeof(values[0])); ++i) {
    ASSERT_EQ(OEMU_OK, oemu_sysreg_write(&sr_, OEMU_SYSREG_TTBR0_EL1, values[i]));
    ASSERT_EQ(OEMU_OK, oemu_sysreg_write(&sr_, OEMU_SYSREG_TTBR1_EL1, values[i]));
    uint64_t low = 0U;
    uint64_t high = 0U;
    ASSERT_EQ(OEMU_OK, oemu_sysreg_read(&sr_, OEMU_SYSREG_TTBR0_EL1, &low));
    ASSERT_EQ(OEMU_OK, oemu_sysreg_read(&sr_, OEMU_SYSREG_TTBR1_EL1, &high));
    EXPECT_EQ(values[i], low) << "TTBR0 lost or narrowed the write";
    EXPECT_EQ(values[i], high) << "TTBR1 lost or narrowed the write";
  }
}

}  // namespace

TEST_F(SysregTest, FpStatusRegistersAreModelledAndThirtyTwoBitsWide) {
  // FPCR/FPSR are not optional here: 6.6's fpsimd_load_state / fpsimd_save_state
  // run on every return to user mode and issue `mrs x0, fpcr` / `msr fpcr, x8`
  // unconditionally, because system_supports_fpsimd() is merely
  // !have_cpucap(ARM64_HAS_NO_FPSIMD) and that cap is a dummy nothing can set.
  // Refusing the selectors therefore kills /init before it prints a byte.
  uint64_t value = 0U;
  ASSERT_EQ(OEMU_OK, oemu_sysreg_read(&sr_, OEMU_SYSREG_FPCR, &value));
  EXPECT_EQ(value, 0U);                 /* both reset to zero, as the oracle's A53 shows */
  ASSERT_EQ(OEMU_SYSREG_FPCR, 0x1a20U); /* the selector the guest actually emits */
  ASSERT_EQ(OEMU_SYSREG_FPSR, 0x1a21U);
  // Only the low 32 bits exist; the write mask is the architecture's, not ours.
  ASSERT_EQ(OEMU_OK, oemu_sysreg_write(&sr_, OEMU_SYSREG_FPCR, UINT64_C(0xFFFF0000FF00FFFF)));
  ASSERT_EQ(OEMU_OK, oemu_sysreg_read(&sr_, OEMU_SYSREG_FPCR, &value));
  EXPECT_EQ(value, UINT64_C(0xFF00FFFF));
  ASSERT_EQ(OEMU_OK, oemu_sysreg_write(&sr_, OEMU_SYSREG_FPSR, UINT64_C(0x100000001)));
  ASSERT_EQ(OEMU_OK, oemu_sysreg_read(&sr_, OEMU_SYSREG_FPSR, &value));
  EXPECT_EQ(value, 1U);
  // And they are EL0-accessible, which is what makes `msr fpcr` from a freestanding
  // /init (or a kernel running at EL1 with EL1FFI... nothing) legal.
  oemu_sysregs el0{};
  oemu_regs regs0{};
  BootAt(&el0, &regs0, OEMU_EL0, 0x2000U);
  ASSERT_EQ(OEMU_OK, oemu_sysreg_write(&el0, OEMU_SYSREG_FPCR, 0x8U));
  ASSERT_EQ(OEMU_OK, oemu_sysreg_read(&el0, OEMU_SYSREG_FPCR, &value));
  EXPECT_EQ(value, 8U);
}
