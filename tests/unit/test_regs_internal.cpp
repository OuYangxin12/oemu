// White-box tests: the register module's pure decision logic, reached through
// src/regs/regs_internal.h.
//
// These three functions carry the parts of the architecture that are easy to get
// subtly wrong, and all of them are pure -- so the condition table can be
// enumerated in full (16 conditions x 16 flag combinations) rather than sampled,
// and the carry/overflow rules can be pinned at their exact boundaries.
#include "oemu/regs.h"

#include <cstdint>

#include <gtest/gtest.h>

#include "regs/regs_internal.h"

namespace {

// Packs a 4-bit NZCV nibble into its architectural position (bits 31..28).
constexpr std::uint32_t Flags(unsigned nibble) {
  return static_cast<std::uint32_t>(nibble) << 28U;
}

constexpr unsigned kCondCount = 16;
constexpr unsigned kFlagCombinations = 16;

// --- truncate ----------------------------------------------------------------

TEST(RegsInternalTruncate, W64IsIdentity) {
  EXPECT_EQ(0u, oemu_regs_internal_truncate(0, OEMU_REG_W64));
  EXPECT_EQ(UINT64_MAX, oemu_regs_internal_truncate(UINT64_MAX, OEMU_REG_W64));
  EXPECT_EQ(0xDEADBEEFCAFEBABEULL,
            oemu_regs_internal_truncate(0xDEADBEEFCAFEBABEULL, OEMU_REG_W64));
}

TEST(RegsInternalTruncate, W32KeepsTheLowHalf) {
  EXPECT_EQ(0u, oemu_regs_internal_truncate(0, OEMU_REG_W32));
  EXPECT_EQ(0x00000000FFFFFFFFULL, oemu_regs_internal_truncate(UINT64_MAX, OEMU_REG_W32));
  EXPECT_EQ(0x00000000CAFEBABEULL,
            oemu_regs_internal_truncate(0xDEADBEEFCAFEBABEULL, OEMU_REG_W32));
}

TEST(RegsInternalTruncate, W32BoundaryValues) {
  // Exactly at the 32-bit limit and one past it.
  EXPECT_EQ(0x00000000FFFFFFFFULL,
            oemu_regs_internal_truncate(0x00000000FFFFFFFFULL, OEMU_REG_W32));
  EXPECT_EQ(0u, oemu_regs_internal_truncate(0x0000000100000000ULL, OEMU_REG_W32));
}

// --- condition codes: exhaustive properties ----------------------------------

TEST(RegsInternalCond, AlwaysConditionsHoldForEveryFlagCombination) {
  // NV (0b1111) is "always" in AArch64, not "never". Reading it as never would
  // silently skip instructions, so it is checked across all 16 combinations.
  for (unsigned f = 0; f < kFlagCombinations; ++f) {
    EXPECT_TRUE(oemu_regs_internal_cond_holds(Flags(f), OEMU_COND_AL)) << "flags=" << f;
    EXPECT_TRUE(oemu_regs_internal_cond_holds(Flags(f), OEMU_COND_NV)) << "flags=" << f;
  }
}

TEST(RegsInternalCond, EvenOddPairsAreExactComplements) {
  // Every pair except AL/NV must be mutually exclusive and exhaustive.
  for (unsigned f = 0; f < kFlagCombinations; ++f) {
    for (unsigned c = 0; c < 0xEU; c += 2) {
      const bool base = oemu_regs_internal_cond_holds(Flags(f), static_cast<oemu_cond>(c));
      const bool inverted =
          oemu_regs_internal_cond_holds(Flags(f), static_cast<oemu_cond>(c + 1));
      EXPECT_NE(base, inverted) << "cond=" << c << " flags=" << f;
    }
  }
}

TEST(RegsInternalCond, IgnoresTheReservedLowBits) {
  // Only bits 31..28 may influence the outcome.
  for (unsigned f = 0; f < kFlagCombinations; ++f) {
    const std::uint32_t clean = Flags(f);
    const std::uint32_t noisy = clean | 0x0FFFFFFFU;
    for (unsigned c = 0; c < kCondCount; ++c) {
      const auto cond = static_cast<oemu_cond>(c);
      EXPECT_EQ(oemu_regs_internal_cond_holds(clean, cond),
                oemu_regs_internal_cond_holds(noisy, cond))
          << "cond=" << c << " flags=" << f;
    }
  }
}

// --- condition codes: truth table against the flag definitions ---------------

TEST(RegsInternalCond, MatchesTheArchitecturalTruthTable) {
  // Recomputes each condition from the flag bits independently of the
  // implementation's pair-decoding structure, for all 256 cases.
  for (unsigned f = 0; f < kFlagCombinations; ++f) {
    const std::uint32_t packed = Flags(f);
    const bool n = (packed & OEMU_NZCV_N) != 0U;
    const bool z = (packed & OEMU_NZCV_Z) != 0U;
    const bool c = (packed & OEMU_NZCV_C) != 0U;
    const bool v = (packed & OEMU_NZCV_V) != 0U;

    const bool expected[kCondCount] = {
        z,               // EQ
        !z,              // NE
        c,               // CS
        !c,              // CC
        n,               // MI
        !n,              // PL
        v,               // VS
        !v,              // VC
        c && !z,         // HI
        !c || z,         // LS
        n == v,          // GE
        n != v,          // LT
        !z && (n == v),  // GT
        z || (n != v),   // LE
        true,            // AL
        true,            // NV
    };

    for (unsigned cond = 0; cond < kCondCount; ++cond) {
      EXPECT_EQ(expected[cond],
                oemu_regs_internal_cond_holds(packed, static_cast<oemu_cond>(cond)))
          << "cond=" << cond << " flags=" << f;
    }
  }
}

// --- AddWithCarry: 64-bit boundaries ----------------------------------------

TEST(RegsInternalAlu, PlainAdditionSetsNoFlags) {
  const oemu_alu_result r = oemu_regs_internal_add_with_carry(1, 1, false, OEMU_REG_W64);
  EXPECT_EQ(2u, r.value);
  EXPECT_EQ(0u, r.nzcv);
}

TEST(RegsInternalAlu, UnsignedWrapSetsCarryAndZero) {
  // UINT64_MAX + 1 == 0: carry out, result zero.
  const oemu_alu_result r =
      oemu_regs_internal_add_with_carry(UINT64_MAX, 1, false, OEMU_REG_W64);
  EXPECT_EQ(0u, r.value);
  EXPECT_EQ(OEMU_NZCV_Z | OEMU_NZCV_C, r.nzcv);
}

TEST(RegsInternalAlu, SignedOverflowSetsOverflowAndNegative) {
  // INT64_MAX + 1 wraps into the negative half: V set, C clear.
  const oemu_alu_result r =
      oemu_regs_internal_add_with_carry(0x7FFFFFFFFFFFFFFFULL, 1, false, OEMU_REG_W64);
  EXPECT_EQ(0x8000000000000000ULL, r.value);
  EXPECT_EQ(OEMU_NZCV_N | OEMU_NZCV_V, r.nzcv);
}

TEST(RegsInternalAlu, CarryInIsIncluded) {
  const oemu_alu_result r = oemu_regs_internal_add_with_carry(1, 1, true, OEMU_REG_W64);
  EXPECT_EQ(3u, r.value);
  EXPECT_EQ(0u, r.nzcv);
}

TEST(RegsInternalAlu, CarryInAloneCanProduceTheCarryOut) {
  // The 64-bit path must detect a carry that only appears once carry_in is
  // added: UINT64_MAX + 0 + 1.
  const oemu_alu_result r =
      oemu_regs_internal_add_with_carry(UINT64_MAX, 0, true, OEMU_REG_W64);
  EXPECT_EQ(0u, r.value);
  EXPECT_EQ(OEMU_NZCV_Z | OEMU_NZCV_C, r.nzcv);
}

TEST(RegsInternalAlu, MaxPlusMaxCarriesWithoutOverflowing) {
  // Two negative operands, negative result: C set, V clear.
  const oemu_alu_result r =
      oemu_regs_internal_add_with_carry(UINT64_MAX, UINT64_MAX, false, OEMU_REG_W64);
  EXPECT_EQ(UINT64_MAX - 1, r.value);
  EXPECT_EQ(OEMU_NZCV_N | OEMU_NZCV_C, r.nzcv);
}

TEST(RegsInternalAlu, NegativeSumOfTwoSignedMinimaOverflows) {
  // INT64_MIN + INT64_MIN == 0 with both carry and overflow.
  const oemu_alu_result r = oemu_regs_internal_add_with_carry(
      0x8000000000000000ULL, 0x8000000000000000ULL, false, OEMU_REG_W64);
  EXPECT_EQ(0u, r.value);
  EXPECT_EQ(OEMU_NZCV_Z | OEMU_NZCV_C | OEMU_NZCV_V, r.nzcv);
}

// --- AddWithCarry: subtraction ------------------------------------------------

TEST(RegsInternalAlu, SubtractionBorrowClearsCarry) {
  // 0 - 1: C clear means "borrow occurred".
  const oemu_alu_result r =
      oemu_regs_internal_add_with_carry(0, ~UINT64_C(1), true, OEMU_REG_W64);
  EXPECT_EQ(UINT64_MAX, r.value);
  EXPECT_EQ(OEMU_NZCV_N, r.nzcv);
}

TEST(RegsInternalAlu, SubtractingEqualValuesSetsZeroAndCarry) {
  const std::uint64_t x = 5;
  const oemu_alu_result r = oemu_regs_internal_add_with_carry(x, ~x, true, OEMU_REG_W64);
  EXPECT_EQ(0u, r.value);
  EXPECT_EQ(OEMU_NZCV_Z | OEMU_NZCV_C, r.nzcv);
}

TEST(RegsInternalAlu, SubtractionWithoutBorrowSetsCarry) {
  // 2 - 1 == 1, no borrow.
  const oemu_alu_result r =
      oemu_regs_internal_add_with_carry(2, ~UINT64_C(1), true, OEMU_REG_W64);
  EXPECT_EQ(1u, r.value);
  EXPECT_EQ(OEMU_NZCV_C, r.nzcv);
}

TEST(RegsInternalAlu, SignedComparisonOfExtremesOverflows) {
  // INT64_MIN - 1 underflows: V set, result becomes INT64_MAX.
  const oemu_alu_result r = oemu_regs_internal_add_with_carry(0x8000000000000000ULL,
                                                              ~UINT64_C(1), true, OEMU_REG_W64);
  EXPECT_EQ(0x7FFFFFFFFFFFFFFFULL, r.value);
  EXPECT_EQ(OEMU_NZCV_C | OEMU_NZCV_V, r.nzcv);
}

// --- AddWithCarry: 32-bit width ----------------------------------------------

TEST(RegsInternalAlu, W32UnsignedWrapSetsCarryAndZero) {
  const oemu_alu_result r =
      oemu_regs_internal_add_with_carry(0xFFFFFFFFULL, 1, false, OEMU_REG_W32);
  EXPECT_EQ(0u, r.value);
  EXPECT_EQ(OEMU_NZCV_Z | OEMU_NZCV_C, r.nzcv);
}

TEST(RegsInternalAlu, W32SignedOverflowUsesBit31) {
  const oemu_alu_result r =
      oemu_regs_internal_add_with_carry(0x7FFFFFFFULL, 1, false, OEMU_REG_W32);
  EXPECT_EQ(0x80000000ULL, r.value);
  EXPECT_EQ(OEMU_NZCV_N | OEMU_NZCV_V, r.nzcv);
}

TEST(RegsInternalAlu, W32SubtractionBorrowClearsCarry) {
  const oemu_alu_result r =
      oemu_regs_internal_add_with_carry(0, (~UINT64_C(1)) & 0xFFFFFFFFULL, true, OEMU_REG_W32);
  EXPECT_EQ(0xFFFFFFFFULL, r.value);
  EXPECT_EQ(OEMU_NZCV_N, r.nzcv);
}

TEST(RegsInternalAlu, W32ResultNeverExceeds32Bits) {
  const oemu_alu_result r =
      oemu_regs_internal_add_with_carry(UINT64_MAX, UINT64_MAX, true, OEMU_REG_W32);
  EXPECT_EQ(0xFFFFFFFFULL, r.value);
  EXPECT_EQ(0u, r.value >> 32U);
}

TEST(RegsInternalAlu, W32IgnoresTheUpperHalfOfItsOperands) {
  // High garbage in either operand must not reach the result or the flags.
  const oemu_alu_result clean = oemu_regs_internal_add_with_carry(1, 2, false, OEMU_REG_W32);
  const oemu_alu_result noisy = oemu_regs_internal_add_with_carry(
      0xFFFFFFFF00000001ULL, 0xDEADBEEF00000002ULL, false, OEMU_REG_W32);

  EXPECT_EQ(clean.value, noisy.value);
  EXPECT_EQ(clean.nzcv, noisy.nzcv);
}

TEST(RegsInternalAlu, W32AndW64DisagreeOnTheSameOperands) {
  // 0x7FFFFFFF + 1 overflows at 32 bits but is unremarkable at 64. A shared
  // implementation that forgot the width would fail one of these.
  const oemu_alu_result w32 =
      oemu_regs_internal_add_with_carry(0x7FFFFFFFULL, 1, false, OEMU_REG_W32);
  const oemu_alu_result w64 =
      oemu_regs_internal_add_with_carry(0x7FFFFFFFULL, 1, false, OEMU_REG_W64);

  EXPECT_EQ(OEMU_NZCV_N | OEMU_NZCV_V, w32.nzcv);
  EXPECT_EQ(0u, w64.nzcv);
  EXPECT_EQ(w32.value, w64.value);
}

// --- flag derivation feeding condition evaluation ----------------------------

TEST(RegsInternalAlu, DerivedFlagsDriveSignedComparisons) {
  // CMP 1, 2 -> 1 < 2 signed and unsigned: LT and CC hold, GE and HI do not.
  const oemu_alu_result r =
      oemu_regs_internal_add_with_carry(1, ~UINT64_C(2), true, OEMU_REG_W64);

  EXPECT_TRUE(oemu_regs_internal_cond_holds(r.nzcv, OEMU_COND_LT));
  EXPECT_TRUE(oemu_regs_internal_cond_holds(r.nzcv, OEMU_COND_CC));
  EXPECT_FALSE(oemu_regs_internal_cond_holds(r.nzcv, OEMU_COND_GE));
  EXPECT_FALSE(oemu_regs_internal_cond_holds(r.nzcv, OEMU_COND_HI));
}

TEST(RegsInternalAlu, DerivedFlagsSurviveTheSignedOverflowCase) {
  // CMP INT64_MIN, 1: the subtraction overflows, and GE/LT must still reflect
  // the true signed ordering thanks to the N == V test.
  const oemu_alu_result r = oemu_regs_internal_add_with_carry(0x8000000000000000ULL,
                                                              ~UINT64_C(1), true, OEMU_REG_W64);

  EXPECT_TRUE(oemu_regs_internal_cond_holds(r.nzcv, OEMU_COND_LT));
  EXPECT_FALSE(oemu_regs_internal_cond_holds(r.nzcv, OEMU_COND_GE));
}

}  // namespace
