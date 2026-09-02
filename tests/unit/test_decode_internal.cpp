// White-box tests: the decoder's pure bit primitives, via
// src/decode/decode_internal.h.
//
// These carry the arithmetic that the rest of the decoder trusts, so they are
// tested exhaustively rather than by example. In particular DecodeBitMasks is
// swept across its entire 2^19 input space: most of that space is unallocated,
// and a decoder that accepts a reserved pattern silently invents an immediate.
#include "oemu/decode.h"
#include "oemu/regs.h"
#include "oemu/status.h"

#include <cstdint>

#include <gtest/gtest.h>

#include "decode/decode_internal.h"

namespace {

// --- bit extraction ----------------------------------------------------------

TEST(DecodeInternalBits, ExtractsAtOffsetZero) {
  EXPECT_EQ(0x0Fu, oemu_decode_internal_bits(0xFFFFFF0Fu, 0, 8));
  EXPECT_EQ(0x1u, oemu_decode_internal_bits(0x00000001u, 0, 1));
}

TEST(DecodeInternalBits, ExtractsAtAnOffset) {
  EXPECT_EQ(0x3u, oemu_decode_internal_bits(0x00000030u, 4, 4));
  EXPECT_EQ(0x1Fu, oemu_decode_internal_bits(0xF8000000u, 27, 5));
}

TEST(DecodeInternalBits, ExtractsTheTopBit) {
  EXPECT_EQ(1u, oemu_decode_internal_bits(0x80000000u, 31, 1));
  EXPECT_EQ(0u, oemu_decode_internal_bits(0x7FFFFFFFu, 31, 1));
}

TEST(DecodeInternalBits, FullWidthIsIdentity) {
  // The width == 32 path cannot use a shift-based mask, so it is special-cased.
  EXPECT_EQ(0xDEADBEEFu, oemu_decode_internal_bits(0xDEADBEEFu, 0, 32));
}

// --- sign extension ----------------------------------------------------------

TEST(DecodeInternalSignExtend, PositiveValuesAreUnchanged) {
  EXPECT_EQ(0, oemu_decode_internal_sign_extend(0, 8));
  EXPECT_EQ(1, oemu_decode_internal_sign_extend(1, 8));
  EXPECT_EQ(127, oemu_decode_internal_sign_extend(127, 8));
}

TEST(DecodeInternalSignExtend, NegativeValuesExtend) {
  EXPECT_EQ(-1, oemu_decode_internal_sign_extend(0xFF, 8));
  EXPECT_EQ(-128, oemu_decode_internal_sign_extend(0x80, 8));
  EXPECT_EQ(-2, oemu_decode_internal_sign_extend(0xFE, 8));
}

TEST(DecodeInternalSignExtend, IgnoresBitsAboveTheWidth) {
  // The caller passes a raw field, so anything above `width` must be discarded.
  EXPECT_EQ(1, oemu_decode_internal_sign_extend(0xFFFFFF01u, 8));
  EXPECT_EQ(-1, oemu_decode_internal_sign_extend(0xFFFFFFFFu, 8));
}

TEST(DecodeInternalSignExtend, HandlesASingleBit) {
  EXPECT_EQ(0, oemu_decode_internal_sign_extend(0, 1));
  EXPECT_EQ(-1, oemu_decode_internal_sign_extend(1, 1));
}

TEST(DecodeInternalSignExtend, HandlesTheEncodingWidthsInUse) {
  // 7, 9, 14, 19, 21 and 26 are the widths A64 actually encodes.
  EXPECT_EQ(-64, oemu_decode_internal_sign_extend(0x40, 7));
  EXPECT_EQ(63, oemu_decode_internal_sign_extend(0x3F, 7));
  EXPECT_EQ(-256, oemu_decode_internal_sign_extend(0x100, 9));
  EXPECT_EQ(255, oemu_decode_internal_sign_extend(0xFF, 9));
  EXPECT_EQ(-8192, oemu_decode_internal_sign_extend(0x2000, 14));
  EXPECT_EQ(-262144, oemu_decode_internal_sign_extend(0x40000, 19));
  EXPECT_EQ(-1048576, oemu_decode_internal_sign_extend(0x100000, 21));
  EXPECT_EQ(-33554432, oemu_decode_internal_sign_extend(0x2000000, 26));
}

TEST(DecodeInternalSignExtend, FullWidthIsAReinterpretation) {
  EXPECT_EQ(-1, oemu_decode_internal_sign_extend(UINT64_MAX, 64));
  EXPECT_EQ(INT64_MIN, oemu_decode_internal_sign_extend(0x8000000000000000ULL, 64));
}

TEST(DecodeInternalSignExtend, IsExhaustivelyCorrectAtEightBits) {
  // Cheap to enumerate, and pins the boundary between the two branches.
  for (unsigned v = 0; v < 256; ++v) {
    const int64_t expected =
        (v < 128) ? static_cast<int64_t>(v) : static_cast<int64_t>(v) - 256;
    EXPECT_EQ(expected, oemu_decode_internal_sign_extend(v, 8)) << "value=" << v;
  }
}

// --- rotate ------------------------------------------------------------------

TEST(DecodeInternalRor, ZeroAmountIsIdentity) {
  EXPECT_EQ(0xF0u, oemu_decode_internal_ror(0xF0, 0, 8));
}

TEST(DecodeInternalRor, RotatesWithinTheElement) {
  EXPECT_EQ(0x0Fu, oemu_decode_internal_ror(0xF0, 4, 8));
  EXPECT_EQ(0x87u, oemu_decode_internal_ror(0x0F, 1, 8));
}

TEST(DecodeInternalRor, AmountWrapsModuloElementSize) {
  EXPECT_EQ(oemu_decode_internal_ror(0xF0, 4, 8), oemu_decode_internal_ror(0xF0, 12, 8));
}

TEST(DecodeInternalRor, HandlesA64BitElement) {
  EXPECT_EQ(0x8000000000000000ULL, oemu_decode_internal_ror(1, 1, 64));
  EXPECT_EQ(1u, oemu_decode_internal_ror(1, 64, 64));
}

TEST(DecodeInternalRor, MasksOutBitsAboveTheElement) {
  // A 4-bit element must not see the high nibble.
  EXPECT_EQ(0x1u, oemu_decode_internal_ror(0xF1, 0, 4));
}

// --- DecodeBitMasks ----------------------------------------------------------

TEST(DecodeInternalBitMasks, RejectsNullOutput) {
  EXPECT_EQ(OEMU_ERR_INVALID_ARG,
            oemu_decode_internal_bit_masks(false, 0, 0, OEMU_REG_W64, nullptr));
}

TEST(DecodeInternalBitMasks, DecodesAReplicated32BitElement) {
  // N=0, imms=0b000111, immr=0. N=0 with that imms puts the highest set bit of
  // N:NOT(imms) at position 5, so the element is 32 bits wide and holds a run of
  // 8 ones -- giving 0xFF repeated every 32 bits, not a solid 0xFF..FF.
  uint64_t mask = 0;
  ASSERT_EQ(OEMU_OK, oemu_decode_internal_bit_masks(false, 0x07, 0, OEMU_REG_W64, &mask));
  EXPECT_EQ(0x000000FF000000FFULL, mask);
}

TEST(DecodeInternalBitMasks, NoEncodingProducesAnAllOnesMask) {
  // Verified by sweeping the whole space below: the immediate logical forms
  // cannot express a solid 0xFF..FF, because s == levels is reserved. That is
  // deliberate in the architecture -- an all-ones constant comes from MOVN, or
  // from ORR with XZR. A decoder that "helpfully" accepted the reserved pattern
  // would invent an immediate no real assembler can emit.
  uint64_t mask = 0;
  ASSERT_EQ(OEMU_OK, oemu_decode_internal_bit_masks(true, 0x3E, 0, OEMU_REG_W64, &mask));
  EXPECT_EQ(0x7FFFFFFFFFFFFFFFULL, mask) << "63 ones is the widest run encodable";

  for (unsigned n = 0; n < 2; ++n) {
    for (uint32_t imms = 0; imms < 64; ++imms) {
      for (uint32_t immr = 0; immr < 64; ++immr) {
        uint64_t swept = 0;
        if (oemu_decode_internal_bit_masks(n != 0u, imms, immr, OEMU_REG_W64, &swept) ==
            OEMU_OK) {
          ASSERT_NE(UINT64_MAX, swept) << "n=" << n << " imms=" << imms << " immr=" << immr;
        }
      }
    }
  }
}

TEST(DecodeInternalBitMasks, DecodesTheEncodingUsedByAndImmediate) {
  // From a real `and x0, x1, #0xff`: N=1, immr=0, imms=7.
  uint64_t mask = 0;
  ASSERT_EQ(OEMU_OK, oemu_decode_internal_bit_masks(true, 0x07, 0, OEMU_REG_W64, &mask));
  EXPECT_EQ(0xFFULL, mask);
}

TEST(DecodeInternalBitMasks, RotationMovesTheRun) {
  uint64_t unrotated = 0;
  uint64_t rotated = 0;
  ASSERT_EQ(OEMU_OK, oemu_decode_internal_bit_masks(true, 0x07, 0, OEMU_REG_W64, &unrotated));
  ASSERT_EQ(OEMU_OK, oemu_decode_internal_bit_masks(true, 0x07, 4, OEMU_REG_W64, &rotated));
  EXPECT_NE(unrotated, rotated);
  // Rotating an 8-bit run right by 4 within a 64-bit element.
  EXPECT_EQ(0xF00000000000000FULL, rotated);
}

TEST(DecodeInternalBitMasks, RejectsAnAllOnesElement) {
  // s == levels is reserved: that pattern is what MOVN exists for.
  uint64_t mask = 0;
  EXPECT_EQ(OEMU_ERR_DECODE,
            oemu_decode_internal_bit_masks(true, 0x3F, 0, OEMU_REG_W64, &mask));
}

TEST(DecodeInternalBitMasks, RejectsNSetAt32BitWidth) {
  // A 64-bit element cannot fit in a 32-bit register.
  uint64_t mask = 0;
  EXPECT_EQ(OEMU_ERR_DECODE,
            oemu_decode_internal_bit_masks(true, 0x07, 0, OEMU_REG_W32, &mask));
}

TEST(DecodeInternalBitMasks, ProducesNonZeroMasksForEveryAcceptedInput) {
  // An immediate logical operation with a zero mask would be meaningless, so no
  // accepted encoding may yield one. Sweeping the full space also proves the
  // function terminates and stays in range everywhere.
  unsigned accepted = 0;
  unsigned rejected = 0;
  for (int width_index = 0; width_index < 2; ++width_index) {
    const oemu_reg_width width = (width_index == 0) ? OEMU_REG_W32 : OEMU_REG_W64;
    const unsigned reg_bits = (width == OEMU_REG_W32) ? 32u : 64u;
    for (unsigned n = 0; n < 2; ++n) {
      for (uint32_t imms = 0; imms < 64; ++imms) {
        for (uint32_t immr = 0; immr < 64; ++immr) {
          uint64_t mask = 0;
          const oemu_status status =
              oemu_decode_internal_bit_masks(n != 0u, imms, immr, width, &mask);
          if (status == OEMU_OK) {
            ++accepted;
            EXPECT_NE(0u, mask) << "n=" << n << " imms=" << imms << " immr=" << immr;
            if (reg_bits == 32u) {
              EXPECT_EQ(0u, mask >> 32u) << "32-bit mask leaked into the high half";
            }
          } else {
            ++rejected;
            EXPECT_EQ(OEMU_ERR_DECODE, status);
          }
        }
      }
    }
  }
  // Both outcomes must be well represented; a decoder that accepted everything
  // or nothing would still pass the per-case assertions above.
  EXPECT_GT(accepted, 1000u);
  EXPECT_GT(rejected, 1000u);
  EXPECT_EQ(2u * 2u * 64u * 64u, accepted + rejected);
}

TEST(DecodeInternalBitMasks, A32BitMaskNeverLeaksIntoTheHighHalf) {
  // The same encoding at 32-bit width: a single 32-bit element, so the result is
  // just the run of ones and the upper half must be clear.
  uint64_t mask = 0;
  ASSERT_EQ(OEMU_OK, oemu_decode_internal_bit_masks(false, 0x07, 0, OEMU_REG_W32, &mask));
  EXPECT_EQ(0xFFULL, mask);
  EXPECT_EQ(0u, mask >> 32u);
}

// --- extend amount ----------------------------------------------------------

TEST(DecodeInternalExtendAmount, AcceptsZeroThroughFour) {
  for (unsigned amount = 0; amount <= 4; ++amount) {
    EXPECT_TRUE(oemu_decode_internal_extend_amount_valid(amount)) << amount;
  }
}

TEST(DecodeInternalExtendAmount, RejectsFiveAndAbove) {
  EXPECT_FALSE(oemu_decode_internal_extend_amount_valid(5));
  EXPECT_FALSE(oemu_decode_internal_extend_amount_valid(7));
}

}  // namespace
