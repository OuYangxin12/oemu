/*
 * White-box tests for src/exec/exec_internal.h: the pure operand helpers with
 * every edge enumerated, and a handful of dispatch cases that are painful to
 * reach from a hand-assembled program (crafted oemu_insn values).
 */
#include "oemu/decode.h"
#include "oemu/exec.h"
#include "oemu/memory.h"
#include "oemu/regs.h"

#include <array>

#include <gtest/gtest.h>

#include "exec/exec_internal.h"
#include "support/tracking_allocator.h"

namespace {

/* --- shifted operand ------------------------------------------------------------- */

struct ShiftCase {
  uint64_t value;
  oemu_shift_type type;
  unsigned amount;
  oemu_reg_width width;
  uint64_t value_expect;
  bool carry_valid;
  bool carry_expect;
};

class ShiftTable : public ::testing::TestWithParam<ShiftCase> {};

TEST_P(ShiftTable, Operand) {
  const ShiftCase &c = GetParam();
  const oemu_exec_shift_result r =
      oemu_exec_internal_shift_operand(c.value, c.type, c.amount, c.width);
  EXPECT_EQ(r.value, c.value_expect);
  EXPECT_EQ(r.carry_valid, c.carry_valid);
  EXPECT_EQ(r.carry, c.carry_expect);
}

INSTANTIATE_TEST_SUITE_P(
    ShiftEdges, ShiftTable,
    ::testing::Values(
        /* imm6 == 0: four different architectural quirks. */
        ShiftCase{0xFULL, OEMU_SHIFT_LSL, 0U, OEMU_REG_W64, 0xFULL, false, false},
        ShiftCase{UINT64_C(0x8000000000000000), OEMU_SHIFT_LSL, 0U, OEMU_REG_W64,
                  UINT64_C(0x8000000000000000), false, false},
        ShiftCase{0xFULL, OEMU_SHIFT_LSR, 0U, OEMU_REG_W64, 0ULL, true, false},
        ShiftCase{UINT64_C(0x8000000000000000), OEMU_SHIFT_LSR, 0U, OEMU_REG_W64, 0ULL, true,
                  true},
        ShiftCase{0xFULL, OEMU_SHIFT_ASR, 0U, OEMU_REG_W64, 0ULL, true, false},
        ShiftCase{UINT64_C(0x8000000000000000), OEMU_SHIFT_ASR, 0U, OEMU_REG_W64, ~0ULL, true,
                  true},
        ShiftCase{UINT64_C(0x8000000000000000), OEMU_SHIFT_ROR, 0U, OEMU_REG_W64, ~0ULL, true,
                  true},
        ShiftCase{UINT64_C(0x7FFFFFFFFFFFFFFF), OEMU_SHIFT_ROR, 0U, OEMU_REG_W64, 0ULL, true,
                  false},
        /* ordinary amounts: carry is the last bit shifted out. */
        ShiftCase{UINT64_C(0x8000000000000000), OEMU_SHIFT_LSL, 1U, OEMU_REG_W64, 0ULL, true,
                  true},
        ShiftCase{0x3ULL, OEMU_SHIFT_LSR, 1U, OEMU_REG_W64, 0x1ULL, true, true},
        ShiftCase{0x4ULL, OEMU_SHIFT_LSR, 3U, OEMU_REG_W64, 0ULL, true, true},
        ShiftCase{0x80000000ULL, OEMU_SHIFT_ASR, 4U, OEMU_REG_W32, 0xF8000000U, true, false},
        ShiftCase{0xFULL, OEMU_SHIFT_ROR, 4U, OEMU_REG_W64, UINT64_C(0xF000000000000000), true,
                  true},
        ShiftCase{0xFULL, OEMU_SHIFT_ROR, 4U, OEMU_REG_W32, 0xF0000000U, true, true},
        /* W32: the result must not spill above bit 31. */
        ShiftCase{UINT64_C(0x1000000FF), OEMU_SHIFT_LSL, 1U, OEMU_REG_W32, 0x1FEU, true, false},
        ShiftCase{0xFFFFFFFFU, OEMU_SHIFT_ASR, 1U, OEMU_REG_W32, 0xFFFFFFFFU, true, true}));

/* --- extended operand ------------------------------------------------------------- */

TEST(ExecInternal, ExtendOperand) {
  EXPECT_EQ(oemu_exec_internal_extend_operand(UINT64_C(0x1122334455667788), OEMU_EXTEND_UXTB,
                                              0U, false),
            0x88ULL);
  EXPECT_EQ(oemu_exec_internal_extend_operand(UINT64_C(0x1122334455667788), OEMU_EXTEND_UXTH,
                                              0U, false),
            0x7788ULL);
  EXPECT_EQ(oemu_exec_internal_extend_operand(UINT64_C(0x1122334455667788), OEMU_EXTEND_UXTW,
                                              0U, false),
            0x55667788ULL);
  EXPECT_EQ(oemu_exec_internal_extend_operand(UINT64_C(0x1122334455667788), OEMU_EXTEND_SXTB,
                                              0U, false),
            ~0x77ULL);
  EXPECT_EQ(oemu_exec_internal_extend_operand(UINT64_C(0x1122334455667788), OEMU_EXTEND_SXTH,
                                              0U, false),
            0x7788ULL); /* bit 15 is clear: SXTH does not extend it */
  EXPECT_EQ(oemu_exec_internal_extend_operand(UINT64_C(0xFFFFFFFF80000000), OEMU_EXTEND_SXTW,
                                              0U, false),
            UINT64_C(0xFFFFFFFF80000000));
  EXPECT_EQ(oemu_exec_internal_extend_operand(UINT64_C(0x1122334455667788), OEMU_EXTEND_SXTX,
                                              0U, false),
            UINT64_C(0x1122334455667788));
  /* the extension happens first, the shift second */
  EXPECT_EQ(oemu_exec_internal_extend_operand(0x88ULL, OEMU_EXTEND_UXTB, 3U, false), 0x440ULL);
  /* the UXTX+LSL form is a plain left shift of the full index */
  EXPECT_EQ(oemu_exec_internal_extend_operand(UINT64_C(0x1122334455667788), OEMU_EXTEND_UXTX,
                                              3U, true),
            UINT64_C(0x89119A22AB33BC40));
}

/* --- NZ ----------------------------------------------------------------------------- */

TEST(ExecInternal, NzTestsZeroAtTheOperandWidth) {
  EXPECT_EQ(oemu_exec_internal_nz(0ULL, OEMU_REG_W32), OEMU_NZCV_Z);
  EXPECT_EQ(oemu_exec_internal_nz(UINT64_C(0x100000000), OEMU_REG_W32),
            OEMU_NZCV_Z); /* W result truncates to zero */
  EXPECT_EQ(oemu_exec_internal_nz(UINT64_C(0x100000000), OEMU_REG_W64), 0U);
  EXPECT_EQ(oemu_exec_internal_nz(0x80000000U, OEMU_REG_W32), OEMU_NZCV_N);
  EXPECT_EQ(oemu_exec_internal_nz(0x80000000U, OEMU_REG_W64), 0U);
  EXPECT_EQ(oemu_exec_internal_nz(~0ULL, OEMU_REG_W64), OEMU_NZCV_N);
  EXPECT_EQ(oemu_exec_internal_nz(0x80000001U, OEMU_REG_W32), OEMU_NZCV_N);
}

/* --- wide multiplies ------------------------------------------------------------------ */

class MulhTable : public ::testing::TestWithParam<std::array<uint64_t, 3>> {};

TEST_P(MulhTable, Golden) {
  const std::array<uint64_t, 3> &c = GetParam();
  EXPECT_EQ(oemu_exec_internal_umulh(c[0], c[1]), c[2]);
}

INSTANTIATE_TEST_SUITE_P(
    UmulhGolden, MulhTable,
    ::testing::Values(
        std::array<uint64_t, 3>{{0ULL, 0ULL, 0ULL}},
        std::array<uint64_t, 3>{{~0ULL, ~0ULL, UINT64_C(0xFFFFFFFFFFFFFFFE)}},
        std::array<uint64_t, 3>{{UINT64_C(0x8000000000000000), 2ULL, 1ULL}}, /* 2^64: hi = 1 */
        std::array<uint64_t, 3>{{UINT64_C(0xFFFFFFFFFFFFFFFF), 2ULL, 1ULL}},
        std::array<uint64_t, 3>{{UINT64_C(0x123456789ABCDEF), UINT64_C(0xFEDCBA9876543210),
                                 UINT64_C(0x0121FA00AD77D742)}}));

TEST(ExecInternal, SmulhAppliesTheSignCorrections) {
  EXPECT_EQ(oemu_exec_internal_smulh(~0ULL, ~0ULL), 0ULL); /* (-1)(-1)=1 */
  EXPECT_EQ(oemu_exec_internal_smulh(0ULL, ~0ULL), 0ULL);
  EXPECT_EQ(
      oemu_exec_internal_smulh(UINT64_C(0x8000000000000000), UINT64_C(0x8000000000000000)),
      UINT64_C(0x4000000000000000)); /* 2^126 >> 64 */
  EXPECT_EQ(oemu_exec_internal_smulh(UINT64_C(0x8000000000000000), ~0ULL),
            0ULL); /* (-2^63)(-1) */
  EXPECT_EQ(oemu_exec_internal_smulh(UINT64_C(0x8000000000000000), 0ULL), 0ULL);
  EXPECT_EQ(oemu_exec_internal_smulh(~0ULL, 2ULL),
            ~0ULL); /* (-1)(2) = -2: high word all-ones */
}

/* --- one-source helpers ----------------------------------------------------------------- */

TEST(ExecInternal, ClzAndClsAtTheEdges) {
  EXPECT_EQ(oemu_exec_internal_clz(0ULL, OEMU_REG_W64), 64ULL);
  EXPECT_EQ(oemu_exec_internal_clz(~0ULL, OEMU_REG_W64), 0ULL);
  EXPECT_EQ(oemu_exec_internal_clz(1ULL, OEMU_REG_W64), 63ULL);
  EXPECT_EQ(oemu_exec_internal_clz(0ULL, OEMU_REG_W32), 32ULL);
  EXPECT_EQ(oemu_exec_internal_clz(0xFFFFFFFFULL, OEMU_REG_W32), 0ULL);
  EXPECT_EQ(oemu_exec_internal_clz(UINT64_C(0x100000000), OEMU_REG_W32),
            32ULL); /* truncates: W sees only the zeroes */

  EXPECT_EQ(oemu_exec_internal_cls(0ULL, OEMU_REG_W64), 0ULL);
  EXPECT_EQ(oemu_exec_internal_cls(~0ULL, OEMU_REG_W64), 0ULL);
  EXPECT_EQ(oemu_exec_internal_cls(1ULL, OEMU_REG_W64), 62ULL);
  EXPECT_EQ(oemu_exec_internal_cls(UINT64_C(0x4000000000000000), OEMU_REG_W64), 0ULL);
  EXPECT_EQ(oemu_exec_internal_cls(UINT64_C(0x7FFFFFFFFFFFFFFF), OEMU_REG_W64), 0ULL);
  EXPECT_EQ(oemu_exec_internal_cls(UINT64_C(0xFFFFFFFF80000000), OEMU_REG_W32), 0ULL);
}

TEST(ExecInternal, BitReversers) {
  EXPECT_EQ(oemu_exec_internal_rbit(1ULL, OEMU_REG_W64), UINT64_C(0x8000000000000000));
  EXPECT_EQ(oemu_exec_internal_rbit(0ULL, OEMU_REG_W64), 0ULL);
  EXPECT_EQ(oemu_exec_internal_rbit(0xFFFFFFFFULL, OEMU_REG_W32), 0xFFFFFFFFULL);
  EXPECT_EQ(oemu_exec_internal_rbit(1ULL, OEMU_REG_W32), 0x80000000U);
  EXPECT_EQ(oemu_exec_internal_rev(UINT64_C(0x0102030405060708), OEMU_REG_W64),
            UINT64_C(0x0807060504030201));
  EXPECT_EQ(oemu_exec_internal_rev(0x12345678U, OEMU_REG_W32), 0x78563412U);
  EXPECT_EQ(oemu_exec_internal_rev(0x1ULL, OEMU_REG_W32), 0x01000000U);
  EXPECT_EQ(oemu_exec_internal_rev16(UINT64_C(0x0102030405060708), OEMU_REG_W64),
            UINT64_C(0x0201040306050807));
  EXPECT_EQ(oemu_exec_internal_rev16(0x12345678U, OEMU_REG_W32), 0x34127856U);
  EXPECT_EQ(oemu_exec_internal_rev32(UINT64_C(0xAABBCCDD11223344)),
            UINT64_C(0x11223344AABBCCDD));
}

/* --- dispatch, crafted ---------------------------------------------------------------------
 */

class DispatchCrafted : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(oemu_memory_init(&mem_, 4U), OEMU_OK);
    ASSERT_EQ(oemu_memory_map(&mem_, kData, 0x1000U, OEMU_PERM_ALL), OEMU_OK);
    oemu_sysenv_init(&env_, nullptr);
    ASSERT_EQ(oemu_cpu_init(&cpu_, kPc, kPc + 0x8000U), OEMU_OK);
  }
  void TearDown() override {
    oemu_memory_dispose(&mem_);
    EXPECT_FALSE(tracker_.has_leaks());
  }

  /* A decoder-fresh instruction, then the fields a crafted case cares about. */
  oemu_insn base(oemu_opcode op) {
    oemu_insn in{};
    in.word = 0U;
    in.op = op;
    in.width = OEMU_REG_W64;
    in.operand_kind = OEMU_OPERAND_NONE;
    in.cond = OEMU_COND_AL;
    in.rn = 1U;
    in.rm = 2U;
    in.rd = 0U;
    in.ra = 3U;
    in.rt2 = 4U;
    return in;
  }

  oemu_status dispatch(oemu_insn *in) {
    return oemu_exec_internal_dispatch(&cpu_, &mem_, &env_, in);
  }
  uint64_t x(unsigned n) { return oemu_regs_read(&cpu_.regs, n, OEMU_REG_W64); }
  void set_x(unsigned n, uint64_t v) { oemu_regs_write(&cpu_.regs, n, OEMU_REG_W64, v); }

  static constexpr uint64_t kPc = UINT64_C(0x400000);
  static constexpr uint64_t kData = UINT64_C(0x500000);

  oemu_memory mem_{};
  oemu_sysenv env_{};
  oemu_cpu cpu_{};
  oemu_test::TrackingAllocator tracker_;
};

TEST_F(DispatchCrafted, RefusesMissingOrUnknownInstructions) {
  oemu_insn in = base(OEMU_OP_ADD);
  EXPECT_EQ(oemu_exec_internal_dispatch(nullptr, &mem_, &env_, &in), OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(oemu_exec_internal_dispatch(&cpu_, nullptr, &env_, &in), OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(oemu_exec_internal_dispatch(&cpu_, &mem_, &env_, nullptr), OEMU_ERR_INVALID_ARG);
  in.op = OEMU_OP_UNKNOWN;
  EXPECT_EQ(dispatch(&in), OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(oemu_regs_pc(&cpu_.regs), kPc); /* a refusal moves nothing */
}

TEST_F(DispatchCrafted, LogicalAllOnesImmediateKeepsCarrySet) {
  /* No assembler in this environment prints the all-ones logical immediate,
   * so the mask rule is pinned down here instead: ANDS with an all-ones
   * immediate always leaves C set (the ARM-mandated quirk). */
  oemu_insn in = base(OEMU_OP_ANDS);
  in.operand_kind = OEMU_OPERAND_IMM;
  in.uimm = ~0ULL;
  set_x(1, 0xF0U);
  oemu_regs_set_nzcv(&cpu_.regs, 0U);
  ASSERT_EQ(dispatch(&in), OEMU_OK);
  EXPECT_EQ(x(0), 0xF0U);
  EXPECT_EQ(oemu_regs_nzcv(&cpu_.regs), OEMU_NZCV_C);
}

TEST_F(DispatchCrafted, LogicalOrdinaryImmediateClearsCarry) {
  oemu_insn in = base(OEMU_OP_ANDS);
  in.operand_kind = OEMU_OPERAND_IMM;
  in.uimm = 0xF0U; /* not the all-ones mask: C is forced low */
  set_x(1, 0xF0U);
  oemu_regs_set_nzcv(&cpu_.regs, OEMU_NZCV_C);
  ASSERT_EQ(dispatch(&in), OEMU_OK);
  EXPECT_EQ(oemu_regs_nzcv(&cpu_.regs), 0U); /* result nonzero, no flags at all */
}

TEST_F(DispatchCrafted, CcmpConditionFalseInjectsTheEncodedFlags) {
  oemu_insn in = base(OEMU_OP_CCMP);
  in.cond = OEMU_COND_EQ;
  in.operand_kind = OEMU_OPERAND_IMM;
  in.imm = 5;
  in.uimm = 5;
  oemu_regs_set_nzcv(&cpu_.regs, OEMU_NZCV_N | OEMU_NZCV_V);
  ASSERT_EQ(dispatch(&in), OEMU_OK);
  EXPECT_EQ(oemu_regs_nzcv(&cpu_.regs), OEMU_NZCV_Z | OEMU_NZCV_V); /* the 0b0101 */
}

TEST_F(DispatchCrafted, BranchCommitWritesLinkBeforeTarget) {
  oemu_insn in = base(OEMU_OP_BL);
  in.imm = static_cast<int64_t>(kPc + 0x40U);
  ASSERT_EQ(dispatch(&in), OEMU_OK);
  EXPECT_EQ(oemu_regs_pc(&cpu_.regs), kPc + 0x40U);
  EXPECT_EQ(x(30), kPc + 4U);

  in = base(OEMU_OP_RET);
  in.rn = 30U;
  ASSERT_EQ(dispatch(&in), OEMU_OK);
  EXPECT_EQ(oemu_regs_pc(&cpu_.regs), kPc + 4U);
}

TEST_F(DispatchCrafted, NonBranchDispatchAlwaysAdvancesPcByFour) {
  oemu_insn in = base(OEMU_OP_NOP);
  ASSERT_EQ(dispatch(&in), OEMU_OK);
  EXPECT_EQ(oemu_regs_pc(&cpu_.regs), kPc + 4U);
  in = base(OEMU_OP_HINT);
  ASSERT_EQ(dispatch(&in), OEMU_OK);
  in = base(OEMU_OP_BARRIER);
  ASSERT_EQ(dispatch(&in), OEMU_OK);
  EXPECT_EQ(oemu_regs_pc(&cpu_.regs), kPc + 12U);
}

TEST_F(DispatchCrafted, BreakIsAFaultAndPrecise) {
  oemu_insn in = base(OEMU_OP_BRK);
  in.uimm = 1U;
  set_x(0, 0xAU);
  EXPECT_EQ(dispatch(&in), OEMU_ERR_FAULT);
  EXPECT_EQ(oemu_regs_pc(&cpu_.regs), kPc);
  EXPECT_EQ(x(0), 0xAU);
  in.op = OEMU_OP_HLT;
  EXPECT_EQ(dispatch(&in), OEMU_ERR_FAULT);
}

}  // namespace
