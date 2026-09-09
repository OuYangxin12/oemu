// Black-box tests for exception delivery and return.
//
// The expected values are composed from the architectural constants (or cited
// literals verified against QEMU's arm_cpu_do_interrupt_aarch64 and syndrome.h
// plus Linux's esr.h), never from the implementation, so a regression in the
// entry sequence shows up as a value mismatch rather than a tautology.
#include "oemu/exc.h"
#include "oemu/regs.h"
#include "oemu/sysreg.h"

#include <cstdint>

#include <gtest/gtest.h>

namespace {

// Boot PSTATE composed the way oemu_sysregs_init does: mode, SPSel above EL0,
// all four DAIF masks set. Keeping the composition here (instead of hex
// literals) makes an unexpected PSTATE readable in a failure diff.
uint64_t BootPstate(oemu_el el) {
  uint64_t pstate = oemu_pstate_mode(el) | (OEMU_PSTATE_DAIF_MASK << OEMU_PSTATE_DAIF_SHIFT);
  if (el != OEMU_EL0) {
    pstate |= OEMU_PSTATE_SPSEL;
  }
  return pstate;
}

// Entry PSTATE: h-mode of the target, DAIF all set, IL set.
uint64_t EntryPstate(oemu_el el) {
  return oemu_pstate_mode(el) | (OEMU_PSTATE_DAIF_MASK << OEMU_PSTATE_DAIF_SHIFT) |
         OEMU_PSTATE_IL;
}

constexpr uint64_t kIlBit = 1U << 25;  // ESR IL: raised by a 32-bit instruction

// ESR word base for a class: EC in bits [31:26]. The explicit uint32_t keeps
// the shift out of int's range.
constexpr uint32_t EcBase(oemu_exc_ec ec) {
  return static_cast<uint32_t>(ec) << 26U;
}

class ExcTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(OEMU_OK, oemu_regs_init(&regs_, 0x1000, 0x41000000));
    oemu_sysregs_init(&sr_, &regs_, OEMU_EL1);
    // A non-zero vector base so delivered PCs are recognisable in assertions.
    sr_.vbar_el[OEMU_EL1] = 0x8000;
    sr_.vbar_el[OEMU_EL3] = 0x80000;
  }

  static void BootAt(oemu_regs *regs, oemu_sysregs *sr, oemu_el el, uint64_t pc, uint64_t sp) {
    ASSERT_EQ(OEMU_OK, oemu_regs_init(regs, pc, sp));
    oemu_sysregs_init(sr, regs, el);
  }

  oemu_regs regs_{};
  oemu_sysregs sr_{};
};

// --- routing and vector layout -------------------------------------------------

TEST_F(ExcTest, RoutesEverythingToEl1ExceptEl3) {
  // Documented simplification (roadmap D3): no EL2, no SCR routing; EL3 is
  // the top of the model so its exceptions stay there.
  EXPECT_EQ(OEMU_EL1, oemu_exc_route(OEMU_EL0));
  EXPECT_EQ(OEMU_EL1, oemu_exc_route(OEMU_EL1));
  EXPECT_EQ(OEMU_EL3, oemu_exc_route(OEMU_EL3));
}

TEST_F(ExcTest, VectorOffsetsFollowTheArchitecturalGroups) {
  // Lower-EL group is fixed at 0x400 (AArch64); the same-EL group picks 0x000
  // or 0x200 on the interrupted SPSel. Inside a group the four entries are in
  // the architectural order Synchronous, System error, IRQ, FIQ -- which is
  // NOT the order of oemu_exc_kind, so the table below spells the slots out
  // rather than striding by the enum value. An earlier version of this test
  // asserted the enum order (IRQ at +0x080), and that is what it was wrong
  // about: it was written against the implementation, and the implementation
  // was delivering every IRQ to the System-error vector.
  const struct {
    bool same_el;
    bool sp_sel;
    oemu_exc_kind kind;
    uint64_t offset;
  } cases[] = {
      {false, false, OEMU_EXC_KIND_SYNC, 0x400},  {false, false, OEMU_EXC_KIND_SERROR, 0x480},
      {false, false, OEMU_EXC_KIND_IRQ, 0x500},   {false, false, OEMU_EXC_KIND_FIQ, 0x580},
      {false, true, OEMU_EXC_KIND_SYNC, 0x400},   {true, false, OEMU_EXC_KIND_SYNC, 0x000},
      {true, false, OEMU_EXC_KIND_SERROR, 0x080}, {true, false, OEMU_EXC_KIND_IRQ, 0x100},
      {true, false, OEMU_EXC_KIND_FIQ, 0x180},    {true, true, OEMU_EXC_KIND_SYNC, 0x200},
      {true, true, OEMU_EXC_KIND_SERROR, 0x280},  {true, true, OEMU_EXC_KIND_IRQ, 0x300},
      {true, true, OEMU_EXC_KIND_FIQ, 0x380},
  };
  for (const auto &c : cases) {
    EXPECT_EQ(c.offset, oemu_exc_vector_offset(c.same_el, c.sp_sel, c.kind))
        << "same_el=" << c.same_el << " sp_sel=" << c.sp_sel
        << " kind=" << static_cast<int>(c.kind);
  }
}

// --- entry: state transitions ----------------------------------------------------

TEST_F(ExcTest, SvcFromEl0DeliversToTheLowerElVectorGroup) {
  oemu_regs regs{};
  oemu_sysregs sr{};
  BootAt(&regs, &sr, OEMU_EL0, 0x3000, 0x2000);
  // VBAR_EL1 is still 0 at boot -- the vector is vbar + 0x400 + 0x000.

  oemu_exc_svc(&regs, &sr, 0x42);

  EXPECT_EQ(0x400U, regs.pc);
  EXPECT_EQ(EntryPstate(OEMU_EL1), sr.pstate);
  // The interrupted world is recorded in EL1's banks.
  EXPECT_EQ(0x3000U, sr.elr_el[OEMU_EL1]);
  EXPECT_EQ(BootPstate(OEMU_EL0), sr.spsr_el[OEMU_EL1]);
  EXPECT_EQ(EcBase(OEMU_EXC_EC_SVC64) | kIlBit | 0x42U, sr.esr_el[OEMU_EL1]);
  // FAR is not an SVC attribute and must stay untouched.
  EXPECT_EQ(0U, sr.far_el[OEMU_EL1]);
  // The user stack moved into SP_EL0's bank; the kernel got its (still
  // zeroed) SP_EL1.
  EXPECT_EQ(0x2000U, sr.sp_el[OEMU_EL0]);
  EXPECT_EQ(0U, regs.sp);
}

TEST_F(ExcTest, UndefFromEl1LandsOnTheSameElSpElxVector) {
  oemu_exc_undefined(&regs_, &sr_, 0x00000000U);

  EXPECT_EQ(0x8200U, regs_.pc);  // vbar 0x8000 + 0x200 (same EL, SPSel=1)
  EXPECT_EQ(EntryPstate(OEMU_EL1), sr_.pstate);
  EXPECT_EQ(0x1000U, sr_.elr_el[OEMU_EL1]);
  EXPECT_EQ(BootPstate(OEMU_EL1), sr_.spsr_el[OEMU_EL1]);
  EXPECT_EQ(kIlBit, sr_.esr_el[OEMU_EL1]);  // EC 0b000000, ISS = insn[24:0] = 0
  // SP stays in SP_EL1's bank: same bank, save-then-load is a no-op.
  EXPECT_EQ(0x41000000U, regs_.sp);
  EXPECT_EQ(0x41000000U, sr_.sp_el[OEMU_EL1]);
}

TEST_F(ExcTest, El3KeepsItsOwnExceptions) {
  oemu_regs regs{};
  oemu_sysregs sr{};
  BootAt(&regs, &sr, OEMU_EL3, 0x9000, 0x2000);
  sr.vbar_el[OEMU_EL3] = 0x80000;

  oemu_exc_svc(&regs, &sr, 0);

  EXPECT_EQ(0x80200U, regs.pc);  // vbar_el3 0x80000 + 0x200 (same EL, SPSel=1)
  EXPECT_EQ(EntryPstate(OEMU_EL3), sr.pstate);
  EXPECT_EQ(0x9000U, sr.elr_el[OEMU_EL3]);
  EXPECT_EQ(BootPstate(OEMU_EL3), sr.spsr_el[OEMU_EL3]);
  EXPECT_EQ(0x2000U, sr.sp_el[OEMU_EL3]);
}

TEST_F(ExcTest, TakeWithIrqKindLeavesEsrAndFarAlone) {
  // IRQ carries no syndrome: the architecture leaves ESR_ELx and FAR_ELx as
  // they were, and the vCPU (M2c) will rely on that not clobbering a
  // handler's ESR.
  ASSERT_EQ(OEMU_OK, oemu_sysreg_write(&sr_, OEMU_SYSREG_ESR_EL1, 0x1234));

  oemu_exc_take(&regs_, &sr_, OEMU_EXC_KIND_IRQ, OEMU_EL1, 0xDEADBEEFU, 0x6000, false);

  EXPECT_EQ(0x8000U + 0x200U + 0x100U, regs_.pc);  // IRQ slot of the same-EL group
  EXPECT_EQ(0x1234U, sr_.esr_el[OEMU_EL1]);
  EXPECT_EQ(0U, sr_.far_el[OEMU_EL1]);
}

TEST_F(ExcTest, SerrorWritesEsr) {
  const uint32_t esr = EcBase(OEMU_EXC_EC_SERROR) | kIlBit;
  oemu_exc_take(&regs_, &sr_, OEMU_EXC_KIND_SERROR, OEMU_EL1, esr, 0x6000, false);

  EXPECT_EQ(0x8000U + 0x200U + 0x080U, regs_.pc);  // System-error slot of the same-EL group
  EXPECT_EQ(esr, sr_.esr_el[OEMU_EL1]);
}

TEST_F(ExcTest, FarIsWrittenOnlyWhenValid) {
  oemu_exc_take(&regs_, &sr_, OEMU_EXC_KIND_SYNC, OEMU_EL1,
                EcBase(OEMU_EXC_EC_DABORT_SAME) | kIlBit, 0x6000, true);
  EXPECT_EQ(0x6000U, sr_.far_el[OEMU_EL1]);
}

TEST_F(ExcTest, EntryAlwaysRecordsIlInSpsrOnNestedExceptions) {
  oemu_exc_undefined(&regs_, &sr_, 0);  // entry: SPSR = boot PSTATE (IL=0)
  oemu_exc_undefined(&regs_, &sr_, 0);  // nested: SPSR = entry PSTATE (IL=1)
  EXPECT_EQ(OEMU_PSTATE_IL, sr_.spsr_el[OEMU_EL1] & OEMU_PSTATE_IL);
}

// --- entry: abort helpers ---------------------------------------------------------

TEST_F(ExcTest, DataAbortFromEl1UsesSameElClassAndCarriesIssDetails) {
  oemu_exc_data_abort(&regs_, &sr_, 0x6000, 3, true, true, 0x0C);

  EXPECT_EQ(0x8200U, regs_.pc);
  EXPECT_EQ(0x6000U, sr_.far_el[OEMU_EL1]);
  const uint32_t expected = EcBase(OEMU_EXC_EC_DABORT_SAME) | kIlBit | (1U << 24)  // ISV
                            | (3U << 21)  // SAS for 8 bytes
                            | (1U << 11)  // SET = 0b01 (whole access)
                            | (1U << 6)   // WnR
                            | 0x0C;       // DFSC: permission fault, level 0
  EXPECT_EQ(expected, sr_.esr_el[OEMU_EL1]);
}

TEST_F(ExcTest, DataAbortWithoutIssAdvertisesOnlyDfsc) {
  oemu_exc_data_abort(&regs_, &sr_, 0x6000, 2, false, false, 0x01);

  const uint32_t expected = EcBase(OEMU_EXC_EC_DABORT_SAME) | kIlBit | 0x01U;
  EXPECT_EQ(expected, sr_.esr_el[OEMU_EL1]);
}

TEST_F(ExcTest, DataAbortFromEl0UsesTheLowerElClass) {
  oemu_regs regs{};
  oemu_sysregs sr{};
  BootAt(&regs, &sr, OEMU_EL0, 0x1000, 0x2000);

  oemu_exc_data_abort(&regs, &sr, 0x4000, 0, false, true, 0x21);

  EXPECT_EQ(EcBase(OEMU_EXC_EC_DABORT_LOWER) | kIlBit | (1U << 24) | (1U << 11) | 0x21U,
            sr.esr_el[OEMU_EL1]);
  EXPECT_EQ(0x4000U, sr.far_el[OEMU_EL1]);
  EXPECT_EQ(0x400U, regs.pc);  // lower-EL group, sync slot
}

TEST_F(ExcTest, InstructionAbortUsesTranslationFaultLevelMinus1) {
  oemu_exc_instruction_abort(&regs_, &sr_, 0x1000);

  EXPECT_EQ(0x1000U, sr_.far_el[OEMU_EL1]);
  EXPECT_EQ(EcBase(OEMU_EXC_EC_IABORT_SAME) | kIlBit | 0x2CU, sr_.esr_el[OEMU_EL1]);
}

TEST_F(ExcTest, SvcAndBrkCarryTheirImmediateInIss) {
  oemu_exc_svc(&regs_, &sr_, 0x1234);
  EXPECT_EQ(EcBase(OEMU_EXC_EC_SVC64) | kIlBit | 0x1234U, sr_.esr_el[OEMU_EL1]);

  // Reset to a fresh boot world for the second delivery.
  SetUp();
  oemu_exc_brk(&regs_, &sr_, 0x800);
  EXPECT_EQ(EcBase(OEMU_EXC_EC_BRK64) | kIlBit | 0x800U, sr_.esr_el[OEMU_EL1]);
}

TEST_F(ExcTest, SmcAndHvcDeliverUndefinedUntilPsciExists) {
  oemu_exc_smc(&regs_, &sr_, 0x42);
  EXPECT_EQ(kIlBit | (0xD4000843U & 0x01FFFFFFU), sr_.esr_el[OEMU_EL1]);

  SetUp();
  oemu_exc_hvc(&regs_, &sr_, 0x42);
  EXPECT_EQ(kIlBit | (0xD4000842U & 0x01FFFFFFU), sr_.esr_el[OEMU_EL1]);
}

// --- ERET -------------------------------------------------------------------------

TEST_F(ExcTest, EretRestoresTheInterruptedWorld) {
  const uint64_t boot_pstate = sr_.pstate;

  oemu_exc_svc(&regs_, &sr_, 0);
  ASSERT_EQ(0x8200U, regs_.pc);  // same-EL sync slot (SPSel was 1)
  ASSERT_EQ(EntryPstate(OEMU_EL1), sr_.pstate);

  oemu_exc_eret(&regs_, &sr_);

  EXPECT_EQ(0x1000U, regs_.pc);  // ELR_EL1
  EXPECT_EQ(boot_pstate, sr_.pstate);
  EXPECT_EQ(0x41000000U, regs_.sp);
  EXPECT_EQ(0U, sr_.pstate & OEMU_PSTATE_IL);
}

TEST_F(ExcTest, EretFromEl0RestoresTheUserStack) {
  oemu_regs regs{};
  oemu_sysregs sr{};
  BootAt(&regs, &sr, OEMU_EL0, 0x3000, 0x2000);

  oemu_exc_svc(&regs, &sr, 0);
  ASSERT_EQ(0x2000U, sr.sp_el[OEMU_EL0]) << "entry must preserve SP_EL0";
  regs.sp = 0x9C00;  // the handler runs on SP_EL1

  // The handler restores its saved context by hand, then ERETs.
  sr.elr_el[OEMU_EL1] = 0x5000;
  sr.spsr_el[OEMU_EL1] = BootPstate(OEMU_EL0);
  oemu_exc_eret(&regs, &sr);

  EXPECT_EQ(0x5000U, regs.pc);
  EXPECT_EQ(BootPstate(OEMU_EL0), sr.pstate);
  EXPECT_EQ(0x2000U, regs.sp) << "ERET to EL0t must swap back to SP_EL0";
}

TEST_F(ExcTest, EretWithIlSetDeliversIllegalEret) {
  oemu_exc_svc(&regs_, &sr_, 0);
  // A kernel that smashed its saved context: IL set in the SPSR it is about
  // to return through.
  sr_.spsr_el[OEMU_EL1] |= OEMU_PSTATE_IL;

  oemu_exc_eret(&regs_, &sr_);

  EXPECT_EQ(EcBase(OEMU_EXC_EC_ILLEGAL_ERET) | kIlBit, sr_.esr_el[OEMU_EL1]);
  EXPECT_EQ(0x8200U, regs_.pc);  // re-entered the same-EL sync vector
  EXPECT_EQ(OEMU_PSTATE_IL, sr_.pstate & OEMU_PSTATE_IL);
}

TEST_F(ExcTest, EretToAHigherElIsUndefined) {
  sr_.spsr_el[OEMU_EL1] =
      OEMU_PSTATE_M_EL3H | (OEMU_PSTATE_DAIF_MASK << OEMU_PSTATE_DAIF_SHIFT);

  oemu_exc_eret(&regs_, &sr_);

  EXPECT_EQ(kIlBit | (0xD69F03E0U & 0x01FFFFFFU), sr_.esr_el[OEMU_EL1]);
  EXPECT_EQ(0x8200U, regs_.pc);
  EXPECT_EQ(OEMU_PSTATE_M_EL1H, sr_.pstate & OEMU_PSTATE_M_MASK) << "the EL1 world is intact";
}

TEST_F(ExcTest, EretToAarch32OrReservedModesIsUndefined) {
  const uint64_t bad_modes[] = {
      0x14U,  // EL1t with the AArch32 bit set
      0x01U,  // EL0h: EL0 only exists as EL0t
      0x02U,  // bit 1 is RES0
      0x13U,  // AArch32 SVC mode
  };
  for (const uint64_t mode : bad_modes) {
    SetUp();
    sr_.spsr_el[OEMU_EL1] = mode | (OEMU_PSTATE_DAIF_MASK << OEMU_PSTATE_DAIF_SHIFT);
    oemu_exc_eret(&regs_, &sr_);
    EXPECT_EQ(kIlBit | (0xD69F03E0U & 0x01FFFFFFU), sr_.esr_el[OEMU_EL1])
        << "mode " << mode << " must be rejected";
  }
}

TEST_F(ExcTest, EretToEl0tEl1tAndEl1hAreTheLegalTargetsFromEl1) {
  // Returning upward (EL2/EL3 from EL1) is rejected by the higher-EL rule,
  // so from EL1 the legal modes are exactly EL0t, EL1t and EL1h.
  const uint64_t legal[] = {0x00U, 0x04U, 0x05U};
  for (const uint64_t mode : legal) {
    SetUp();
    sr_.spsr_el[OEMU_EL1] = mode | (OEMU_PSTATE_DAIF_MASK << OEMU_PSTATE_DAIF_SHIFT);
    oemu_exc_eret(&regs_, &sr_);
    EXPECT_EQ(mode, sr_.pstate & OEMU_PSTATE_M_MASK) << "mode " << mode << " must restore";
    EXPECT_EQ(0U, sr_.pstate & OEMU_PSTATE_IL);
    EXPECT_NE(kIlBit | (0xD69F03E0U & 0x01FFFFFFU), sr_.esr_el[OEMU_EL1]);
  }
}

// --- diagnostics -------------------------------------------------------------------

TEST_F(ExcTest, EcNamesAreStableAndNeverNull) {
  EXPECT_STREQ("Unknown", oemu_exc_ec_name(OEMU_EXC_EC_UNKNOWN));
  EXPECT_STREQ("SVC64", oemu_exc_ec_name(OEMU_EXC_EC_SVC64));
  EXPECT_STREQ("BRK64", oemu_exc_ec_name(OEMU_EXC_EC_BRK64));
  EXPECT_STREQ("IllegalERET", oemu_exc_ec_name(OEMU_EXC_EC_ILLEGAL_ERET));
  EXPECT_STREQ("DAbortLower", oemu_exc_ec_name(OEMU_EXC_EC_DABORT_LOWER));
  EXPECT_STREQ("unknown", oemu_exc_ec_name(static_cast<oemu_exc_ec>(0x3FU)));
}

TEST_F(ExcTest, TakingAnInterruptAtEl1hStoresTheFrameOnTheLiveStack) {
  // PSTATE.SP=1 at EL1 makes SP_EL1 the active stack, so an exception entry must
  // keep using the SP the guest last moved and must not resurrect a stale bank
  // copy. On the M5 boot the raw sp_el[OEMU_EL1] slot kept holding the boot
  // stack (0x4ffffff0) long after the kernel had moved to a task stack, which
  // looks alarming in a trace: it is benign only because the entry path keeps
  // the live SP (asserted below) and because the guest cannot read SP_EL1 at
  // EL1 to notice (MRS SP_EL1 is Undefined below EL2, so it never reaches the
  // stale slot either).
  oemu_regs_set_sp(&regs_, 0x41000000ULL);
  sr_.sp_el[OEMU_EL1] = 0x4ffffff0ULL;
  oemu_exc_take(&regs_, &sr_, OEMU_EXC_KIND_IRQ, OEMU_EL1, 0U, 0U, false);
  EXPECT_EQ(0x8300U, regs_.pc);  // IRQ slot of the same-EL group, not System error
  EXPECT_EQ(0x41000000ULL, oemu_regs_sp(&regs_));
  EXPECT_EQ(0x1000U, sr_.elr_el[OEMU_EL1]);
  // Crossing into another bank and back deposits the live value, so the slot is
  // refreshed rather than left as whatever the boot path stored there.
  oemu_sysregs_switch_sp(&sr_, OEMU_PSTATE_M_EL0T);
  oemu_sysregs_switch_sp(&sr_, OEMU_PSTATE_M_EL1H);
  EXPECT_EQ(0x41000000ULL, sr_.sp_el[OEMU_EL1]);
}

}  // namespace
