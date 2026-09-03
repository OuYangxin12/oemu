/*
 * Black-box tests for the executor: programs are assembled from instruction
 * words that were verified against the host assembler's own output, run one
 * oemu_exec_step at a time, and checked register by register.
 *
 * Register discipline used throughout: x1..x6 carry inputs the test sets up
 * front, x0 is a destination, x28 is a branch/sentinel target. `brk #0`
 * (0xd4200000) ends any program that must stop on its own: it surfaces as
 * OEMU_ERR_FAULT with the state left precise, which is itself asserted below.
 */
#include "oemu/exec.h"
#include "oemu/memory.h"
#include "oemu/regs.h"
#include "oemu/sysenv.h"

#include <array>
#include <cstddef>
#include <cstdio>
#include <initializer_list>
#include <string>

#include <gtest/gtest.h>

#include "support/tracking_allocator.h"

namespace {

class ExecTest : public ::testing::Test {
 protected:
  static constexpr uint64_t kText = UINT64_C(0x400000);
  static constexpr uint64_t kData = UINT64_C(0x500000);
  static constexpr uint64_t kStack = UINT64_C(0x600000);

  void SetUp() override {
    ASSERT_EQ(oemu_memory_init(&mem_, 8U), OEMU_OK);
    /* R|W|X so the loader can place programs; executability itself is probed
     * in FetchFaults against the deliberately non-executable data region. */
    ASSERT_EQ(oemu_memory_map(&mem_, kText, 0x1000U, OEMU_PERM_ALL), OEMU_OK);
    ASSERT_EQ(oemu_memory_map(&mem_, kData, 0x1000U, OEMU_PERM_READ | OEMU_PERM_WRITE),
              OEMU_OK);
    ASSERT_EQ(oemu_memory_map(&mem_, kStack, 0x1000U, OEMU_PERM_READ | OEMU_PERM_WRITE),
              OEMU_OK);
    out_ = tmpfile();
    ASSERT_NE(out_, nullptr);
    oemu_sysenv_init(&env_, out_);
    ASSERT_EQ(oemu_cpu_init(&cpu_, kText, kStack + 0x800U), OEMU_OK);
    allocations_at_setup_ = tracker_.alloc_count();
  }
  void TearDown() override {
    if (out_ != nullptr) {
      std::fclose(out_);
    }
    oemu_memory_dispose(&mem_);
    EXPECT_FALSE(tracker_.has_leaks());
    /* The step loop must run the machine without touching the allocator. */
    EXPECT_EQ(tracker_.alloc_count(), allocations_at_setup_);
  }

  /* Places a program at kText and rewinds the PC onto it: tests reprogram
   * mid-case, and leaving the PC anywhere else would execute stale words. */
  void program(std::initializer_list<uint32_t> words) {
    uint64_t addr = kText;
    for (const uint32_t w : words) {
      ASSERT_EQ(oemu_memory_write(&mem_, addr, OEMU_MEM_WORD, w), OEMU_OK);
      addr += OEMU_INSN_SIZE;
    }
    oemu_regs_set_pc(&cpu_.regs, kText);
  }
  void place(uint64_t addr, std::initializer_list<uint32_t> words) {
    for (const uint32_t w : words) {
      ASSERT_EQ(oemu_memory_write(&mem_, addr, OEMU_MEM_WORD, w), OEMU_OK);
      addr += OEMU_INSN_SIZE;
    }
  }
  void store64(uint64_t addr, uint64_t value) {
    ASSERT_EQ(oemu_memory_write(&mem_, addr, OEMU_MEM_DWORD, value), OEMU_OK);
  }
  uint64_t load64(uint64_t addr) {
    uint64_t v = 0U;
    EXPECT_EQ(oemu_memory_read(&mem_, addr, OEMU_MEM_DWORD, false, &v), OEMU_OK);
    return v;
  }

  oemu_status step(oemu_insn *insn = nullptr) {
    return oemu_exec_step(&cpu_, &mem_, &env_, insn);
  }
  oemu_status run(uint64_t budget, uint64_t *done = nullptr) {
    return oemu_exec_run(&cpu_, &mem_, &env_, budget, done);
  }

  /* Runs n instructions; every one of them must succeed. */
  void step_ok(size_t n) {
    for (size_t i = 0U; i < n; i++) {
      ASSERT_EQ(step(), OEMU_OK) << "instruction " << i;
    }
  }

  uint64_t x(unsigned n) { return oemu_regs_read(&cpu_.regs, n, OEMU_REG_W64); }
  uint32_t w(unsigned n) { return (uint32_t)oemu_regs_read(&cpu_.regs, n, OEMU_REG_W32); }
  void set_x(unsigned n, uint64_t value) {
    oemu_regs_write(&cpu_.regs, n, OEMU_REG_W64, value);
  }
  uint64_t sp() { return oemu_regs_sp(&cpu_.regs); }
  uint64_t pc() { return oemu_regs_pc(&cpu_.regs); }
  uint32_t nzcv() { return oemu_regs_nzcv(&cpu_.regs); }
  void set_flags(uint32_t flags) { oemu_regs_set_nzcv(&cpu_.regs, flags); }

  oemu_memory mem_{};
  oemu_sysenv env_{};
  oemu_cpu cpu_{};
  FILE *out_ = nullptr;
  oemu_test::TrackingAllocator tracker_;
  std::size_t allocations_at_setup_ = 0U;
};

/* --- arithmetic --------------------------------------------------------------- */

TEST_F(ExecTest, AddRegisterAndImmediate) {
  program({0x8b020020U, /* add x0,x1,x2 */ 0x91004020U /* add x0,x1,#16 */});
  set_x(1, 5U);
  set_x(2, 7U);
  step_ok(2);
  EXPECT_EQ(x(0), 21U);
  EXPECT_EQ(pc(), kText + 8U);
}

TEST_F(ExecTest, SubSpAdjustsStackPointerAndCmpSpOnlySetsFlags) {
  /* SUB (immediate) with Rd=31 means SP: the real prologue adjustment. */
  program({0xd10083ffU}); /* sub sp,sp,#32 */
  const uint64_t old_sp = sp();
  step_ok(1);
  EXPECT_EQ(sp(), old_sp - UINT64_C(32));
  /* SUBS with Rd=31 and flags set is CMP: SP stays put, flags tell the story. */
  program({0xf10083ffU}); /* subs xzr,sp,#32 -- i.e. cmp sp,#32 */
  const uint64_t still = sp();
  oemu_regs_set_pc(&cpu_.regs, kText);
  step_ok(1);
  EXPECT_EQ(sp(), still);
  EXPECT_EQ(nzcv(), OEMU_NZCV_C); /* no borrow */
}

TEST_F(ExecTest, AddsDetectsSignedOverflow) {
  program({0xab020020U}); /* adds x0,x1,x2 */
  set_x(1, UINT64_C(0x7FFFFFFFFFFFFFFB));
  set_x(2, 7U);
  step_ok(1);
  EXPECT_EQ(x(0), UINT64_C(0x8000000000000002));
  EXPECT_EQ(nzcv(), OEMU_NZCV_N | OEMU_NZCV_V);
}

TEST_F(ExecTest, CmpAndCmnSetFlagsWithoutWritingARegister) {
  program({0xeb02003fU, 0xb1001c3fU}); /* cmp x1,x2 ; cmn x1,#7 */
  set_x(1, 9U);
  set_x(2, 9U);
  step_ok(1);
  EXPECT_EQ(nzcv(), OEMU_NZCV_Z | OEMU_NZCV_C);
  set_x(1, UINT64_C(0x7FFFFFFFFFFFFFFB));
  step_ok(1);
  EXPECT_EQ(nzcv(), OEMU_NZCV_N | OEMU_NZCV_V); /* (2^63-6)+7 overflows */
}

TEST_F(ExecTest, AdcAndSbcThreadTheCarry) {
  program({0x9a020020U, 0xba020020U, 0xda020020U, 0xfa020020U});
  /* adc / adcs / sbc / sbcs, all x0,x1,x2 */
  set_x(1, 0x10U);
  set_x(2, 0x20U);
  set_flags(0U);
  step_ok(4);
  EXPECT_EQ(x(0), UINT64_C(0xFFFFFFFFFFFFFFEF)); /* 0x10 - 0x20 - 0 */
  EXPECT_TRUE((nzcv() & OEMU_NZCV_N) != 0U);     /* the final subtraction borrowed */
  EXPECT_TRUE((nzcv() & OEMU_NZCV_C) == 0U);

  set_flags(OEMU_NZCV_C);
  set_x(0, 0U);
  program({0x9a020020U}); /* adc x0,x1,x2 with C=1 */
  oemu_regs_set_pc(&cpu_.regs, kText);
  step_ok(1);
  EXPECT_EQ(x(0), 0x31U);
}

TEST_F(ExecTest, SubsBorrowClearsCarry) {
  program({0xf10083ffU}); /* cmp sp,#32 with SP at 16: a borrow */
  oemu_regs_set_sp(&cpu_.regs, 16U);
  step_ok(1);
  EXPECT_TRUE((nzcv() & OEMU_NZCV_N) != 0U); /* 16-32 is negative */
  EXPECT_TRUE((nzcv() & OEMU_NZCV_C) == 0U); /* the borrow shows as C clear */
}

/* --- logical ------------------------------------------------------------------ */

TEST_F(ExecTest, TstClearsFlagsItShouldClear) {
  program({0xf2401c1fU}); /* tst x0,#0xff : mask not all-ones -> C cleared */
  set_x(0, 0x100U);
  set_flags(OEMU_NZCV_C);
  step_ok(1);
  EXPECT_EQ(nzcv(), OEMU_NZCV_Z);
}

TEST_F(ExecTest, ShiftedLogicalTakesCarryFromTheShifter) {
  program({0x6aa10c1fU}); /* bics wzr,w0,w1,asr#3 */
  set_x(1, 0x4U);         /* the carry out of an ASR #3 is bit 2 of the source */
  set_flags(0U);
  step_ok(1);
  EXPECT_TRUE((nzcv() & OEMU_NZCV_C) != 0U);
  set_x(1, 0x8U);
  set_flags(OEMU_NZCV_C);
  oemu_regs_set_pc(&cpu_.regs, kText);
  step_ok(1);
  EXPECT_EQ(nzcv() & OEMU_NZCV_C, 0U);
}

TEST_F(ExecTest, ZeroShiftKeepsOldCarry) {
  program({0xea01001fU}); /* tst x0,x1 : ANDS register form, shift LSL#0 */
  set_x(0, 0xF0U);
  set_x(1, 0x0FU);
  set_flags(OEMU_NZCV_C);
  step_ok(1);
  EXPECT_EQ(nzcv(), OEMU_NZCV_Z | OEMU_NZCV_C); /* result 0 -> Z; C untouched */
}

TEST_F(ExecTest, ShiftedOrAndNakedLogicals) {
  program({0xaa421020U, /* orr x0,x1,x2,lsr#4 */ 0xaa220020U, /* orn x0,x1,x2 */
           0xca220020U /* eon x0,x1,x2 */});
  set_x(1, 0x10U);
  set_x(2, 0x100U);
  step_ok(3);
  EXPECT_EQ(x(0), UINT64_C(0xFFFFFFFFFFFFFEEF)); /* 0x10 ^ ~(0x100) */
  program({0x8a040020U});                        /* and x0,x1,x4 */
  oemu_regs_set_pc(&cpu_.regs, kText);
  set_x(4, 0xF0U);
  step_ok(1);
  EXPECT_EQ(x(0), 0x10U);
}

/* --- move wide, adr ------------------------------------------------------------ */

TEST_F(ExecTest, MovWideFamily) {
  program({0xd2824680U, /* mov x0,#0x1234 */ 0x529fffe0U, /* mov w0,#0xffff */
           0x128000a0U,                                   /* mov w0,#-6 (MOVN) */
           0xf2b579a0U});                                 /* movk x0,#0xabcd,lsl#16 */
  step_ok(4);
  EXPECT_EQ(x(0), UINT64_C(0x00000000ABCDFFFA)); /* MOVN #5 -> -6 */
  EXPECT_EQ(w(0), 0xABCDFFFAU);
}

TEST_F(ExecTest, AdrAndAdrpResolveAgainstPc) {
  program({0x10000020U, 0x90000000U, 0x910003e0U}); /* adr x0,+4; adrp x0; mov x0,sp */
  step_ok(1);
  EXPECT_EQ(x(0), kText + 4U);
  step_ok(1);
  EXPECT_EQ(x(0), kText); /* 0x400000 is already page-aligned */
  step_ok(1);
  EXPECT_EQ(x(0), sp());
}

/* --- bitfield ------------------------------------------------------------------- */

TEST_F(ExecTest, SbfmExtractsAndSignExtends) {
  program({0x93485c20U}); /* sbfx x0,x1,#8,#16 */
  set_x(1, UINT64_C(0x123456789ABCDEFF));
  step_ok(1);
  EXPECT_EQ(x(0), UINT64_C(0xFFFFFFFFFFFFBCDE));
}

TEST_F(ExecTest, SbfmPlainRangeAndWrappedForm) {
  program({0x9370dc20U}); /* sbfm x0,x1,#48,#55 (the GAS spelling of sbfiz #48,#8) */
  set_x(1, UINT64_C(0x00FF000000000000)); /* the field itself is negative */
  step_ok(1);
  EXPECT_EQ(x(0), UINT64_MAX);
  program({0x9370dc20U});
  set_x(1, UINT64_C(0x0012000000000000));
  oemu_regs_set_pc(&cpu_.regs, kText);
  step_ok(1);
  EXPECT_EQ(x(0), 0x12U);
}

TEST_F(ExecTest, UbfmCoversUxtwAndUbfiz) {
  program({0xd3407c20U}); /* ubfm x0,x1,#0,#31 (UXTW) */
  set_x(1, UINT64_C(0x1122334455667788));
  step_ok(1);
  EXPECT_EQ(x(0), UINT64_C(0x55667788));
  program({0xd3485c20U}); /* ubfm x0,x1,#8,#23 */
  set_x(1, UINT64_C(0x1122334455667788));
  oemu_regs_set_pc(&cpu_.regs, kText);
  step_ok(1);
  EXPECT_EQ(x(0), 0x6677U);
}

TEST_F(ExecTest, BfmInsertsOnlyItsOwnField) {
  program({0xb3485c20U}); /* bfxil x0,x1,#8,#16 -> BFM #8,#23 */
  set_x(0, UINT64_C(0xDEADBEEF00000000));
  set_x(1, UINT64_C(0x1122334455667788));
  step_ok(1);
  EXPECT_EQ(x(0), UINT64_C(0xDEADBEEF00667700));
}

TEST_F(ExecTest, ExtrConcatenatesThenRotates) {
  program({0x93c22020U}); /* extr x0,x1,x2,#8 */
  set_x(1, UINT64_C(0xFEDCBA9876543210));
  set_x(2, UINT64_C(0x0123456789ABCDEF));
  step_ok(1);
  EXPECT_EQ(x(0), UINT64_C(0x100123456789ABCD));
  program({0x13821020U}); /* extr w0,w1,w2,#4 */
  set_x(1, 0x12345678U);
  set_x(2, 0x9ABCDEF0U);
  oemu_regs_set_pc(&cpu_.regs, kText);
  step_ok(1);
  EXPECT_EQ(x(0), 0x89ABCDEFU);
}

/* --- variable shifts ------------------------------------------------------------- */

TEST_F(ExecTest, VariableShiftsMaskTheAmountAndNeverTouchFlags) {
  program({0x9ac22020U, 0x1ac22420U, 0x9ac22820U, 0x1ac22c20U});
  /* lslv / lsrv(w) / asrv / rorv(w), all dst x0,x1,x2 */
  set_flags(OEMU_NZCV_N);
  set_x(1, 0xF000U);
  set_x(2, 65U); /* 65 mod 64 = 1 */
  step_ok(1);
  EXPECT_EQ(x(0), 0x1E000U);
  set_x(2, 0U);
  step_ok(1);
  EXPECT_EQ(x(0), 0xF000U);
  set_x(1, 0x80000000U);
  set_x(2, 31U);
  step_ok(1);
  EXPECT_EQ(x(0), 1U); /* X form: positive input, plain shift */
  set_x(1, 0x1U);
  set_x(2, 4U);
  step_ok(1);
  EXPECT_EQ(x(0), 0x10000000U);   /* rorv w: right by 4 is left by 28 */
  EXPECT_EQ(nzcv(), OEMU_NZCV_N); /* flags exactly as left */
}

/* --- one-source -------------------------------------------------------------------- */

TEST_F(ExecTest, OneSourceDataProcessing) {
  program({0xdac00020U, /* rbit x0,x1 */ 0x5ac00820U,  /* rev w0,w1 */
           0xdac00c20U, /* rev x0,x1 */ 0xdac00820U,   /* rev32 x0,x1 */
           0xdac00420U, /* rev16 x0,x1 */ 0xdac01020U, /* clz x0,x1 */
           0xdac01420U /* cls x0,x1 */});
  set_x(1, 0x1U);
  step_ok(1);
  EXPECT_EQ(x(0), UINT64_C(0x8000000000000000));
  set_x(1, 0x12345678U);
  step_ok(1);
  EXPECT_EQ(x(0), 0x78563412U);
  set_x(1, UINT64_C(0x0102030405060708));
  step_ok(1);
  EXPECT_EQ(x(0), UINT64_C(0x0807060504030201));
  step_ok(1);
  EXPECT_EQ(x(0), UINT64_C(0x0506070801020304)); /* rev32 swaps the halves */
  step_ok(1);
  EXPECT_EQ(x(0), UINT64_C(0x0201040306050807)); /* rev16 flips each halfword */
  set_x(1, 0U);
  step_ok(1);
  EXPECT_EQ(x(0), 64U);
  step_ok(1);
  EXPECT_EQ(x(0), 0U); /* CLS of all-zeros is 0, not 64 */
}

/* --- multiply and divide ------------------------------------------------------------ */

TEST_F(ExecTest, DivisionNeverFaults) {
  program({0x9ac20820U, /* udiv x0,x1,x2 */ 0x1ac20c20U /* sdiv w0,w1,w2 */});
  set_x(1, 123U);
  set_x(2, 0U);
  step_ok(1);
  EXPECT_EQ(x(0), 0U);
  set_x(1, UINT64_C(0x80000000)); /* INT32_MIN */
  set_x(2, UINT64_C(0xFFFFFFFF)); /* -1 */
  step_ok(1);
  EXPECT_EQ(w(0), 0x80000000U);
  set_x(1, 7U);
  set_x(2, UINT64_C(0xFFFFFFFE)); /* -2 */
  oemu_regs_set_pc(&cpu_.regs, kText + 4U);
  step_ok(1);
  EXPECT_EQ(w(0), 0xFFFFFFFDU); /* truncation toward zero */
}

TEST_F(ExecTest, MulExtendedSaturatesNotAtAllItWraps) {
  program({0x9b020c20U, /* madd x0,x1,x2,x3 */ 0x9b028c20U /* msub x0,x1,x2,x3 */});
  set_x(1, UINT64_C(0x100000000));
  set_x(2, UINT64_C(0x100000000));
  set_x(3, 9U);
  step_ok(1);
  EXPECT_EQ(x(0), 9U); /* product overflowed away, leaving 9 + 0 */
  step_ok(1);
  EXPECT_EQ(x(0), 9U);
}

TEST_F(ExecTest, WideningMultiplyAddsAndSubtractsInSixtyFourBits) {
  program({0x9b220c20U, /* smaddl x0,w1,w2,x3 */ 0x9ba28c20U /* umsubl x0,w1,w2,x3 */});
  set_x(1, UINT64_C(0xFFFFFFFF)); /* w1 = -1 */
  set_x(2, UINT64_C(0x80000000)); /* w2 = INT32_MIN */
  set_x(3, 1U);
  step_ok(1);
  EXPECT_EQ(x(0), UINT64_C(0x80000001)); /* +2^31 + 1 */
  set_x(1, 2U);
  set_x(2, 3U);
  set_x(3, 0U);
  step_ok(1);
  EXPECT_EQ(x(0), UINT64_C(0xFFFFFFFFFFFFFFFA)); /* 0 - 6 */
}

TEST_F(ExecTest, MulHighCoversTheTrickyCorners) {
  program({0x9bc27c20U, /* umulh x0,x1,x2 */ 0x9b427c20U /* smulh x0,x1,x2 */});
  set_x(1, UINT64_MAX);
  set_x(2, UINT64_MAX);
  step_ok(1);
  EXPECT_EQ(x(0), UINT64_C(0xFFFFFFFFFFFFFFFE));
  set_x(1, UINT64_MAX);
  set_x(2, UINT64_MAX);
  step_ok(1);
  EXPECT_EQ(x(0), 0U); /* -1 * -1 = +1 */
  set_x(1, UINT64_C(0x8000000000000000));
  set_x(2, UINT64_C(0x8000000000000000));
  oemu_regs_set_pc(&cpu_.regs, kText + 4U);
  step_ok(1);
  EXPECT_EQ(x(0), UINT64_C(0x4000000000000000)); /* (-2^63)^2 >> 64 = 2^62 */
}

/* --- conditional select and compare --------------------------------------------------- */

TEST_F(ExecTest, CselFamilySelectsByFlagState) {
  program({0x9a820020U, /* csel x0,x1,x2,eq */ 0x1a824420U, /* csinc w0,w1,w2,mi */
           0xda82a020U, /* csinv x0,x1,x2,ge */ 0xda823420U /* csneg x0,x1,x2,lo */});
  set_x(1, 0x100U);
  set_x(2, 0x200U);
  set_flags(0U); /* Z=0 N=0 C=0 V=0 */
  step_ok(1);
  EXPECT_EQ(x(0), 0x200U);
  step_ok(1);
  EXPECT_EQ(x(0), 0x201U); /* MI false -> w2 + 1 */
  step_ok(1);
  EXPECT_EQ(x(0), 0x100U); /* GE true -> x1 */
  step_ok(1);
  EXPECT_EQ(x(0), 0x100U); /* LO true -> x1 */
  program({0xda823420U});  /* csneg, failing condition -> -x2 */
  set_flags(OEMU_NZCV_C);  /* C=1 -> LO false */
  oemu_regs_set_pc(&cpu_.regs, kText);
  step_ok(1);
  EXPECT_EQ(x(0), ~UINT64_C(0x1FF));
  program({0x9a9f47e0U}); /* cset x0,pl */
  set_flags(0U);
  step_ok(1);
  EXPECT_EQ(x(0), 1U);
  program({0x9a9f47e0U});
  set_flags(OEMU_NZCV_N);
  step_ok(1);
  EXPECT_EQ(x(0), 0U);
}

TEST_F(ExecTest, CcmpInjectsItsFlagsWhenTheConditionFails) {
  program({0xfa420024U, /* ccmp x1,x2,#4,eq */ 0xfa431821U, /* ccmp x1,#3,#1,ne */
           0xba426020U /* ccmn x1,x2,#0,vs */});
  set_x(1, 5U);
  set_x(2, 5U);
  set_flags(OEMU_NZCV_Z); /* eq true: a real compare runs */
  step_ok(1);
  EXPECT_EQ(nzcv(), OEMU_NZCV_Z | OEMU_NZCV_C); /* 5-5 */
  set_flags(OEMU_NZCV_Z);                       /* ne false: injected nzcv = 0b0001 = V */
  step_ok(1);
  EXPECT_EQ(nzcv(), OEMU_NZCV_V);
  set_flags(0U); /* vs false: injected nzcv = 0b0000 */
  step_ok(1);
  EXPECT_EQ(nzcv(), 0U);
}

/* --- branches --------------------------------------------------------------------------- */

TEST_F(ExecTest, ConditionalBranchTakenAndNot) {
  /* b.eq +0x1A0; the fall-through traps. The target executes a marker. */
  program({0x540006e0U, 0xd4200000U});
  place(kText + 0x0DCU, {0xd2801540U, 0xd4200000U}); /* mov x0,#0xaa; brk */
  set_flags(OEMU_NZCV_Z);
  step_ok(1);
  EXPECT_EQ(pc(), kText + 0x0DCU);
  step_ok(1);
  EXPECT_EQ(x(0), 0xAAU);
  EXPECT_EQ(step(), OEMU_ERR_FAULT); /* the marker's own brk */
  EXPECT_EQ(pc(), kText + 0x0E0U);   /* BRK is precise: PC still on it */

  oemu_regs_set_pc(&cpu_.regs, kText);
  set_flags(0U); /* Z=0: not taken, falls into the trap */
  step_ok(1);
  EXPECT_EQ(pc(), kText + 4U);
  EXPECT_EQ(step(), OEMU_ERR_FAULT);
}

TEST_F(ExecTest, UnconditionalBranchAndLink) {
  program({0x14000039U, 0xd4200000U}); /* b +0xE4 */
  step_ok(1);
  EXPECT_EQ(pc(), kText + 0xE4U);

  oemu_regs_set_pc(&cpu_.regs, kText);
  program({0x94000038U, 0xd4200000U}); /* bl +0xE0 */
  step_ok(1);
  EXPECT_EQ(pc(), kText + 0xE0U);
  EXPECT_EQ(x(30), kText + 4U); /* the link address is the return site */
}

TEST_F(ExecTest, TestBranchesCheckOneBit) {
  program({0x36280680U, 0xd4200000U}); /* tbz w0,#5 +0xD0 */
  place(kText + 0x0D0U, {0xd2801540U, 0xd4200000U});
  set_x(0, 0U);
  step_ok(2);
  EXPECT_EQ(x(0), 0xAAU);

  oemu_regs_set_pc(&cpu_.regs, kText);
  set_x(0, 0x20U); /* bit 5 set -> not taken */
  step_ok(1);
  EXPECT_EQ(pc(), kText + 4U);

  oemu_regs_set_pc(&cpu_.regs, kText);
  program({0x37100660U, 0xd4200000U}); /* tbnz w0,#2 +0xCC */
  place(kText + 0x0CCU, {0xd2801540U, 0xd4200000U});
  set_x(0, 0x4U);
  step_ok(2);
  EXPECT_EQ(x(0), 0xAAU);
}

TEST_F(ExecTest, CompareBranchesOnZero) {
  program({0xb40006c0U, 0xd4200000U}); /* cbz x0 +0xD8 */
  place(kText + 0x0D8U, {0xd2801540U, 0xd4200000U});
  set_x(0, 0U);
  step_ok(2);
  EXPECT_EQ(x(0), 0xAAU);

  program({0x350006a0U, 0xd4200000U}); /* cbnz w0 +0xD4: tests the low word */
  place(kText + 0x0D4U, {0xd2801540U, 0xd4200000U});
  set_x(0, UINT64_C(0x100000000)); /* W is zero, X is not: CBNZ must not fire */
  step_ok(1);
  EXPECT_EQ(pc(), kText + 4U);
  program({0x350006a0U, 0xd4200000U}); /* rewind so the taken path starts fresh */
  place(kText + 0x0D4U, {0xd2801540U, 0xd4200000U});
  set_x(0, 1U);
  step_ok(2);
  EXPECT_EQ(x(0), 0xAAU);
}

TEST_F(ExecTest, IndirectBranchesAndReturn) {
  program({0xd61f0000U}); /* br x0 */
  set_x(0, kText + 0x40U);
  step_ok(1);
  EXPECT_EQ(pc(), kText + 0x40U);

  oemu_regs_set_pc(&cpu_.regs, kText);
  program({0xd63f0020U}); /* blr x1 */
  set_x(1, kText + 0x80U);
  step_ok(1);
  EXPECT_EQ(pc(), kText + 0x80U);
  EXPECT_EQ(x(30), kText + 4U);

  oemu_regs_set_pc(&cpu_.regs, kText);
  program({0xd65f03c0U}); /* ret */
  set_x(30, kText + 0x20U);
  step_ok(1);
  EXPECT_EQ(pc(), kText + 0x20U);
}

/* --- loads and stores ----------------------------------------------------------------------
 */

TEST_F(ExecTest, LoadStoreSizesAllRoundTrip) {
  program({0xf9000041U, /* str x1,[x2] */ 0xb9000441U, /* str w1,[x2,#4] */
           0x39000441U,                                /* strb w1,[x2,#1] */
           0x79000441U,                                /* strh w1,[x2,#2] */
           0xf9400043U,                                /* ldr x3,[x2] */
           0xf9400443U,                                /* ldr x3,[x2,#8] */
           0x39400043U,                                /* ldrb w3,[x2] */
           0x79400443U /* ldrh w3,[x2,#2] */});
  set_x(1, UINT64_C(0x1122334455667788));
  set_x(2, kData);
  step_ok(8);
  /* Byte trail: 88 88 88 77 88 77 66 55 -- every store saw its own width. */
  EXPECT_EQ(load64(kData), UINT64_C(0x5566778877888888));
  EXPECT_EQ(load64(kData + 8U), 0U);
  EXPECT_EQ(w(3), 0x7788U); /* ldrh ran last, so it is what W3 shows */
  program({0x79400443U});
  oemu_regs_set_pc(&cpu_.regs, kText);
  step_ok(1);
  EXPECT_EQ(w(3), 0x7788U);
}

TEST_F(ExecTest, SignedLoadsExtend) {
  program({0x39800020U, /* ldrsb x0,[x1] */ 0x79c00820U, /* ldrsh w0,[x1,#4] */
           0xb9800820U /* ldrsw x0,[x1,#8] */});
  store64(kData, UINT64_C(0x1111FFFF00FF0080)); /* byte -128, half -1 at +4 */
  store64(kData + 8U, UINT64_C(0x80000000));    /* word INT32_MIN at +8 */
  set_x(1, kData);
  step_ok(1);
  EXPECT_EQ(x(0), ~UINT64_C(0x7F)); /* ldrsb of 0x80 */
  step_ok(1);
  EXPECT_EQ(w(0), 0xFFFFFFFFU);
  step_ok(1);
  EXPECT_EQ(x(0), UINT64_C(0xFFFFFFFF80000000));
}

TEST_F(ExecTest, PreIndexAndPostIndexDifferInAddress) {
  program({0xf8410c20U, /* ldr x0,[x1,#16]! */ 0xf8410420U /* ldr x0,[x1],#16 */});
  store64(kData + 0x10U, 0xAU);
  set_x(1, kData);
  step_ok(2);
  EXPECT_EQ(x(0), 0xAU); /* both forms ended up reading kData+0x10 */
  EXPECT_EQ(x(1), kData + 0x20U);
}

TEST_F(ExecTest, RegisterOffsetFormsScaleAndSignTheIndex) {
  program({0xf8626820U, /* ldr x0,[x1,x2] */ 0xf8627820U, /* ldr x0,[x1,x2,lsl#3] */
           0xf862e820U,                                   /* ldr x0,[x1,x2,sxtx] */
           0x38624820U /* ldrb w0,[x1,w2,uxtw] */});
  store64(kData, 0x1U);
  store64(kData + 16U, 0x2U);
  store64(kData + 0xF8U, 0x3U);
  set_x(1, kData);
  set_x(2, 0U);
  step_ok(1);
  EXPECT_EQ(x(0), 0x1U);
  set_x(2, 2U); /* lsl #3: the only legal shift for a 64-bit load */
  step_ok(1);
  EXPECT_EQ(x(0), 0x2U);
  set_x(1, kData + 0x100U);
  set_x(2, UINT64_MAX - 7U); /* sxtx index -8, unscaled */
  step_ok(1);
  EXPECT_EQ(x(0), 0x3U);
  set_x(2, 1U); /* uxtw, no shift: byte offset 1 of the 0x3 qword */
  step_ok(1);
  EXPECT_EQ(x(0), 0x00U);
}

TEST_F(ExecTest, LiteralLoadIsResolvedByTheDecoder) {
  program({0x58000020U, 0x11223344U}); /* ldr x0, +4 : the word itself is data */
  step_ok(1);
  EXPECT_EQ(x(0), UINT64_C(0x11223344));
}

TEST_F(ExecTest, SpBaseUsesTheStackPointerForm) {
  program({0xf90003ffU}); /* str xzr,[sp] */
  oemu_regs_set_sp(&cpu_.regs, kData);
  store64(kData, UINT64_C(0x1122334455667788));
  step_ok(1);
  EXPECT_EQ(load64(kData), 0U);
}

TEST_F(ExecTest, PairsTransferTwoLocations) {
  program({0xa9400440U, /* ldp x0,x1,[x2] */ 0x29410440U /* ldp w0,w1,[x2,#8] */});
  store64(kData, 0xAU);
  store64(kData + 8U, 0xBU);
  set_x(2, kData);
  step_ok(1);
  EXPECT_EQ(x(0), 0xAU);
  EXPECT_EQ(x(1), 0xBU);
  step_ok(1);
  EXPECT_EQ(w(0), 0xBU); /* the W pair reads the word at x2+8 */
  EXPECT_EQ(w(1), 0x0U);

  program({0xa9000440U}); /* stp x0,x1,[x2] */
  set_x(0, 1U);
  set_x(1, 2U);
  oemu_regs_set_pc(&cpu_.regs, kText);
  step_ok(1);
  EXPECT_EQ(load64(kData), 1U);
  EXPECT_EQ(load64(kData + 8U), 2U);
}

TEST_F(ExecTest, PairWritebackForms) {
  program({0xa8c10bfeU}); /* ldp x30,x2,[sp],#16 */
  store64(kData, 0x30U);
  store64(kData + 8U, 0x31U);
  oemu_regs_set_sp(&cpu_.regs, kData);
  step_ok(1);
  EXPECT_EQ(x(30), 0x30U);
  EXPECT_EQ(x(2), 0x31U);
  EXPECT_EQ(sp(), kData + 16U);

  program({0x29be0440U}); /* stp w0,w1,[x2,#-16]! : a W pair, so transfers are 4 bytes */
  set_x(0, 7U);
  set_x(1, 8U);
  set_x(2, kData + 0x100U);
  oemu_regs_set_pc(&cpu_.regs, kText);
  step_ok(1);
  EXPECT_EQ(load64(kData + 0xF0U), UINT64_C(0x0000000800000007));
  EXPECT_EQ(x(2), kData + 0xF0U); /* pre-index writeback lands on the address used */
}

TEST_F(ExecTest, LdpswSignExtendsAWordPair) {
  program({0x69400440U}); /* ldpsw x0,x1,[x2] */
  /* Simpler poison: word 0 = INT32_MIN, word 1 = 0xFFFFFFFF. */
  ASSERT_EQ(oemu_memory_write(&mem_, kData, OEMU_MEM_WORD, 0x80000000U), OEMU_OK);
  ASSERT_EQ(oemu_memory_write(&mem_, kData + 4U, OEMU_MEM_WORD, 0xFFFFFFFFU), OEMU_OK);
  set_x(2, kData);
  step_ok(1);
  EXPECT_EQ(x(0), UINT64_C(0xFFFFFFFF80000000));
  EXPECT_EQ(x(1), UINT64_MAX);
}

/* --- exclusives
 * ------------------------------------------------------------------------------- */

TEST_F(ExecTest, ExclusiveStoreNeedsAnUntouchedReservation) {
  program({0xc85ffc20U, 0xc8037c22U, 0xc8037c22U});
  /* ldaxr x0,[x1] ; stxr w3,x2,[x1] ; stxr again without a fresh reservation */
  store64(kData, 0x11U);
  set_x(1, kData);
  set_x(2, 0x22U);
  step_ok(1);
  EXPECT_EQ(x(0), 0x11U);
  EXPECT_TRUE(cpu_.monitor_valid);
  step_ok(1);
  EXPECT_EQ(w(3), 0U); /* the status register is W-sized: zero means stored */
  EXPECT_EQ(load64(kData), 0x22U);
  EXPECT_FALSE(cpu_.monitor_valid);
  step_ok(1);
  EXPECT_EQ(w(3), 1U);
  EXPECT_EQ(load64(kData), 0x22U);

  /* A plain store between LDXR and STXR kills the reservation. */
  program({0xc85ffc20U, 0xf9000020U, 0xc8037c22U}); /* ldaxr; str x0,[x1]; stxr */
  oemu_regs_set_pc(&cpu_.regs, kText);
  step_ok(3);
  EXPECT_EQ(w(3), 1U);
}

TEST_F(ExecTest, ExclusiveReservationIsWidthSensitive) {
  /* The reservation records the access width: an STXR of a different width
   * fails rather than succeeding on a partial overlap. */
  program({0xc85f7c20U, /* ldxr x0,[x1] */ 0x88037c22U /* stxr w3,w2,[x1] : W-sized */});
  store64(kData, 0x11U);
  set_x(1, kData);
  set_x(2, 0x22U);
  set_x(3, UINT64_C(0xFFFFFFFF00000000));
  step_ok(1);
  EXPECT_EQ(x(0), 0x11U);
  step_ok(1);
  EXPECT_EQ(x(3), 1U); /* W32 status: high bits zeroed even on failure */
  EXPECT_EQ(load64(kData), 0x11U);
  /* A release store over the reservation kills it too. */
  program({0xc85f7c20U, 0x889ffc22U, 0xc8037c22U}); /* ldxr; stlr w2,[x1]; stxr */
  oemu_regs_set_pc(&cpu_.regs, kText);
  step_ok(3);
  EXPECT_EQ(w(3), 1U);
}

/* --- precise faults
 * ---------------------------------------------------------------------------- */

TEST_F(ExecTest, BrkIsAFaultWithPreciseState) {
  program({0xd4200020U}); /* brk #1 */
  set_x(0, 0xDEADBEEFU);
  EXPECT_EQ(step(), OEMU_ERR_FAULT);
  EXPECT_EQ(pc(), kText);       /* PC did not advance past the trap */
  EXPECT_EQ(x(0), 0xDEADBEEFU); /* and nothing else moved */
}

TEST_F(ExecTest, FaultingPostIndexLoadDoesNotWriteBack) {
  program({0xf8410420U});   /* ldr x0,[x1],#16 */
  set_x(1, kData + 0xFFCU); /* the 8-byte read runs off the region */
  set_x(0, 0xAAU);
  EXPECT_EQ(step(), OEMU_ERR_FAULT);
  EXPECT_EQ(x(1), kData + 0xFFCU); /* the writeback is part of the instruction */
  EXPECT_EQ(x(0), 0xAAU);
}

TEST_F(ExecTest, StpFailsAsAWholeInstruction) {
  program({0xa9000440U}); /* stp x0,x1,[x2] */
  set_x(0, 0x1U);
  set_x(1, 0x2U);
  set_x(2, kData + 0xFF8U); /* second half crosses the end of the region */
  EXPECT_EQ(step(), OEMU_ERR_FAULT);
  EXPECT_EQ(load64(kData + 0xFF8U), 0U); /* neither half was written */
  EXPECT_EQ(pc(), kText);
}

TEST_F(ExecTest, FetchFaultsArePrecise) {
  oemu_regs_set_pc(&cpu_.regs, kText + 2U); /* misaligned: a fetch-level fault */
  EXPECT_EQ(step(), OEMU_ERR_FAULT);
  EXPECT_EQ(pc(), kText + 2U);
  oemu_regs_set_pc(&cpu_.regs, UINT64_C(0x900000)); /* unmapped */
  EXPECT_EQ(step(), OEMU_ERR_FAULT);
  oemu_regs_set_pc(&cpu_.regs, kData); /* mapped, but not executable */
  EXPECT_EQ(step(), OEMU_ERR_FAULT);
}

/* --- system
 * -------------------------------------------------------------------------------------- */

TEST_F(ExecTest, SysregWhitelistRoundTrips) {
  program({0xd51b4200U, /* msr nzcv,x0 */ 0xd53b4200U, /* mrs x0,nzcv */
           0xd5384100U,                                /* mrs x0,sp_el0 */
           0xd5184100U,                                /* msr sp_el0,x0 */
           0xd5384240U,                                /* mrs x0,currentel */
           0xd53bd060U,                                /* mrs x0,tpidrro_el0 */
           0xd51bd0e0U,                                /* msr tpidrur_el0,x0 */
           0xd53bd0e0U /* mrs x0,tpidrur_el0 */});
  set_x(0, OEMU_NZCV_N | OEMU_NZCV_C | 0x0FFFFFFFU); /* junk below the flags */
  step_ok(1);
  EXPECT_EQ(nzcv(), OEMU_NZCV_N | OEMU_NZCV_C);
  step_ok(1);
  EXPECT_EQ(x(0), OEMU_NZCV_N | OEMU_NZCV_C);
  step_ok(1);
  EXPECT_EQ(x(0), sp());
  set_x(0, kStack + 0x100U);
  step_ok(1);
  EXPECT_EQ(sp(), kStack + 0x100U);
  step_ok(1);
  EXPECT_EQ(x(0), 0U); /* CurrentEL on an EL0-only model */
  step_ok(1);
  EXPECT_EQ(x(0), 0U); /* TPIDRRO is read-as-zero here */
  set_x(0, 0x77U);
  step_ok(1);
  step_ok(1);
  EXPECT_EQ(x(0), 0x77U);
}

TEST_F(ExecTest, SysregRefusalsAndUnsupportedEncodings) {
  program({0xd53b4400U}); /* mrs x0,fpcr : outside the whitelist */
  set_x(0, 0xAAU);
  EXPECT_EQ(step(), OEMU_ERR_UNSUPPORTED);
  EXPECT_EQ(x(0), 0xAAU); /* a refusal must not touch the destination */
  program({0xd51b4400U}); /* msr fpcr,x0 */
  oemu_regs_set_pc(&cpu_.regs, kText);
  EXPECT_EQ(step(), OEMU_ERR_UNSUPPORTED);
  program({0xd53800a0U}); /* mrs x0,mpidr_el1 : an EL1 register */
  oemu_regs_set_pc(&cpu_.regs, kText);
  EXPECT_EQ(step(), OEMU_ERR_UNSUPPORTED);
  program({0x1e612800U}); /* fadd d0,d0,d1 : the FP subset is absent */
  oemu_regs_set_pc(&cpu_.regs, kText);
  EXPECT_EQ(step(), OEMU_ERR_UNSUPPORTED);
  program({0xffffffffU}); /* op0 == x111: FP space, rejected wholesale */
  oemu_regs_set_pc(&cpu_.regs, kText);
  EXPECT_EQ(step(), OEMU_ERR_UNSUPPORTED);
}

TEST_F(ExecTest, HintsAndBarriersAreInvisible) {
  program({0xd503201fU, 0xd503203fU, 0xd5033bbfU, 0xd5033fdfU});
  /* nop / yield / dmb ish / isb */
  step_ok(4);
  EXPECT_EQ(pc(), kText + 16U);
}

/* --- SVC through the public step
 * ----------------------------------------------------------------- */

TEST_F(ExecTest, SvcWriteReachesTheHostStream) {
  program({0xd4000001U}); /* svc #0 */
  const std::string text = "abc";
  ASSERT_EQ(
      oemu_memory_write_bytes(&mem_, kData + 0x80U,
                              reinterpret_cast<const uint8_t *>(text.data()), text.size()),
      OEMU_OK);
  set_x(0, 1U);
  set_x(1, kData + 0x80U);
  set_x(2, text.size());
  set_x(8, OEMU_SYS_WRITE);
  step_ok(1);
  EXPECT_EQ(x(0), 3U);
  std::fflush(out_);
  std::rewind(out_);
  std::array<char, 8> captured{};
  EXPECT_EQ(std::fread(captured.data(), 1U, 4U, out_), 3U);
  EXPECT_EQ(std::string(captured.data(), 3U), text);
}

TEST_F(ExecTest, SvcExitEndsTheRunSuccessfully) {
  program({0xd2800160U, /* mov x0,#11 */ 0xd2800ba8U, /* mov x8,#93 */ 0xd4000001U});
  uint64_t done = 0U;
  EXPECT_EQ(run(1000U, &done), OEMU_OK);
  EXPECT_TRUE(oemu_sysenv_exited(&env_));
  EXPECT_EQ(oemu_sysenv_exit_code(&env_), 11);
  EXPECT_EQ(done, 3U);
}

TEST_F(ExecTest, SvcWithoutAnEnvironmentIsUnsupported) {
  program({0xd4000001U});
  EXPECT_EQ(oemu_exec_step(&cpu_, &mem_, nullptr, nullptr), OEMU_ERR_UNSUPPORTED);
  EXPECT_EQ(pc(), kText);
}

/* --- the loop
 * -------------------------------------------------------------------------------------- */

TEST_F(ExecTest, RunBudgetTimesOutMidProgram) {
  program({0x14000000U}); /* b . : the oldest spin in the book */
  uint64_t done = 0U;
  EXPECT_EQ(run(1000U, &done), OEMU_ERR_TIMEOUT);
  EXPECT_EQ(done, 1000U);
  EXPECT_EQ(pc(), kText);
  done = 99U;
  EXPECT_EQ(run(0U, &done), OEMU_ERR_TIMEOUT); /* a spent budget is not an error path */
  EXPECT_EQ(done, 0U);
}

TEST_F(ExecTest, RunSurfacesFaultsWithCounts) {
  program({0xd4200020U}); /* brk #1 */
  uint64_t done = 0U;
  EXPECT_EQ(run(1000U, &done), OEMU_ERR_FAULT);
  EXPECT_EQ(done, 0U);
  EXPECT_EQ(pc(), kText);
}

TEST_F(ExecTest, StepExposesTheDecodedInstruction) {
  program({0x8b020020U});
  oemu_insn insn{};
  ASSERT_EQ(step(&insn), OEMU_OK);
  EXPECT_EQ(insn.op, OEMU_OP_ADD);
  EXPECT_EQ(insn.word, 0x8b020020U);
  program({0xffffffffU});
  oemu_regs_set_pc(&cpu_.regs, kText);
  EXPECT_EQ(step(&insn), OEMU_ERR_UNSUPPORTED);
  EXPECT_EQ(insn.op, OEMU_OP_UNKNOWN);
}

TEST_F(ExecTest, NullArgumentsRefused) {
  EXPECT_EQ(oemu_exec_step(nullptr, &mem_, &env_, nullptr), OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(oemu_exec_step(&cpu_, nullptr, &env_, nullptr), OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(oemu_exec_run(nullptr, &mem_, &env_, 10U, nullptr), OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(oemu_cpu_init(nullptr, 0U, 0U), OEMU_ERR_INVALID_ARG);
}

TEST_F(ExecTest, MovWideAssemblersMatchTheExpectedBitPattern) {
  /* Guards the encodings this whole file is written in: the two MOVZs from
   * the verified table must land their immediates in the right registers. */
  program({0xd2800ba8U, 0xd2801540U});
  step_ok(2);
  EXPECT_EQ(x(8), 93U);
  EXPECT_EQ(x(0), 0xAAU);
}

}  // namespace
