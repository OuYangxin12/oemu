// White-box tests for the ESR builders and the mode validator.
//
// These pin the exact bit layout of the syndrome words against the constants
// cross-checked between QEMU's target/arm/syndrome.h and Linux's
// arch/arm64/include/asm/esr.h -- if either source ever disagrees with oemu,
// these are the assertions that catch it.
#include <cstdint>

#include <gtest/gtest.h>

#include "exc/exc_internal.h"

namespace {

constexpr uint32_t kIlBit = 1U << 25;

TEST(ExcInternalEsr, ComposesClassIlAndIss) {
  const uint32_t esr = oemu_exc_internal_esr(OEMU_EXC_EC_SVC64, 0x01FFFFFFU);
  EXPECT_EQ((0x15U << 26) | kIlBit | 0x01FFFFFFU, esr);
}

TEST(ExcInternalEsr, UndefinedCarriesTheInstructionEncoding) {
  // ISS = insn[24:0]: ERET's bit 24 is clear, so the payload is 0x9F03E0.
  EXPECT_EQ(kIlBit | 0x009F03E0U, oemu_exc_internal_esr_undefined(0xD69F03E0U));
  EXPECT_EQ(kIlBit, oemu_exc_internal_esr_undefined(0U));
  // Bits above the ISS field are dropped, not leaked.
  EXPECT_EQ(kIlBit | 0x01FFFFFFU, oemu_exc_internal_esr_undefined(0xFFFFFFFFU));
}

TEST(ExcInternalEsr, Imm16TruncatesToTheArchitecturalField) {
  EXPECT_EQ((0x15U << 26) | kIlBit | 0xABCDU,
            oemu_exc_internal_esr_imm16(OEMU_EXC_EC_SVC64, 0x1ABCDU));
  EXPECT_EQ((0x17U << 26) | kIlBit, oemu_exc_internal_esr_imm16(OEMU_EXC_EC_SMC64, 0U));
}

TEST(ExcInternalEsr, DataAbortLayoutMatchesTheArmArm) {
  // Full-detail form: ISV=1, SAS for 8 bytes, SET=0b01, WnR=1, DFSC in the low
  // byte. QEMU's FIELD() layout: DFSC[5:0], WnR[6], SET[12:11], SAS[23:21],
  // ISV[24].
  const uint32_t write8 =
      oemu_exc_internal_esr_data_abort(OEMU_EXC_EC_DABORT_SAME, 3, true, true, 0x0C);
  EXPECT_EQ((0x25U << 26) | kIlBit | (1U << 24) | (3U << 21) | (1U << 11) | (1U << 6) | 0x0CU,
            write8);

  // Alignment fault on a 2-byte read from the lower EL: only DFSC survives.
  const uint32_t align2 =
      oemu_exc_internal_esr_data_abort(OEMU_EXC_EC_DABORT_LOWER, 0, false, false, 0x01);
  EXPECT_EQ((0x24U << 26) | kIlBit | 0x01U, align2);
}

TEST(ExcInternalEsr, InstructionAbortCarriesOnlyDfsc) {
  const uint32_t esr = oemu_exc_internal_esr_instruction_abort(OEMU_EXC_EC_IABORT_LOWER, 0x2CU);
  EXPECT_EQ((0x20U << 26) | kIlBit | 0x2CU, esr);
}

TEST(ExcInternalEsr, IllegalEretIsItsOwnClassWithEmptyIss) {
  EXPECT_EQ((0x1AU << 26) | kIlBit, oemu_exc_internal_esr_illegal_eret());
}

TEST(ExcInternalMode, OnlyTheImplementedAarch64StatesValidate) {
  // The seven implemented AArch64 modes; everything else -- AArch32 (bit 4),
  // the RES0 bit 1, EL0 in h-form, and the gaps -- must fail validation.
  const bool valid[32] = {
      /* 0x00 */ true,  /* 0x01 */ false, /* 0x02 */ false, /* 0x03 */ false,
      /* 0x04 */ true,  /* 0x05 */ true,  /* 0x06 */ false, /* 0x07 */ false,
      /* 0x08 */ true,  /* 0x09 */ true,  /* 0x0A */ false, /* 0x0B */ false,
      /* 0x0C */ true,  /* 0x0D */ true,  /* 0x0E */ false, /* 0x0F */ false,
      /* 0x10 */ false, /* 0x11 */ false, /* 0x12 */ false, /* 0x13 */ false,
      /* 0x14 */ false, /* 0x15 */ false, /* 0x16 */ false, /* 0x17 */ false,
      /* 0x18 */ false, /* 0x19 */ false, /* 0x1A */ false, /* 0x1B */ false,
      /* 0x1C */ false, /* 0x1D */ false, /* 0x1E */ false, /* 0x1F */ false,
  };
  for (uint32_t mode = 0; mode < 32; mode++) {
    EXPECT_EQ(valid[mode], oemu_exc_internal_mode_valid(mode)) << "mode " << mode;
  }
  // High junk above the mode field must not influence the verdict: the ERET
  // path masks to M[4:0] before calling, but the predicate is honest either
  // way for the architectural bit width.
  EXPECT_TRUE(oemu_exc_internal_mode_valid(0x05U));
  EXPECT_FALSE(oemu_exc_internal_mode_valid(0x15U));
}

}  // namespace
