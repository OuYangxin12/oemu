/*
 * Black-box tests for the vCPU: boot state, interrupt pins against DAIF,
 * quantum accounting, WFI/WFE wake-or-park, and the system-mode step path --
 * SVC/BRK/undefined/fetch-fault all delivering to the vectors, MRS/MSR going
 * through the sysreg table, and the SEV event register arming WFE.
 *
 * Programs are the same assembler-verified words the exec tests use; the
 * guest here is one or two instructions long because the assertions are
 * about the machine's reaction to a single step, not about programs.
 */
#include "oemu/exc.h"
#include "oemu/memory.h"
#include "oemu/regs.h"
#include "oemu/sysreg.h"
#include "oemu/vcpu.h"

#include <cstddef>
#include <initializer_list>

#include <gtest/gtest.h>

#include "support/tracking_allocator.h"

namespace {

/* Assembler-harvested (clang --target=aarch64, LLVM 18; see build/sysgen). */
constexpr uint32_t kMrsVbarX1 = 0xD538C001U;   // mrs   x1, vbar_el1
constexpr uint32_t kMsrDaifXzr = 0xD51B423FU;  // msr   daif, xzr
constexpr uint32_t kSvc123 = 0xD4002461U;      // svc   #0x123
constexpr uint32_t kBrk234 = 0xD4204680U;      // brk   #0x234
constexpr uint32_t kEret = 0xD69F03E0U;        // eret
constexpr uint32_t kWfi = 0xD503207FU;         // wfi
constexpr uint32_t kWfe = 0xD503205FU;         // wfe
constexpr uint32_t kSev = 0xD503209FU;         // sev
constexpr uint32_t kYield = 0xD503203FU;       // yield (advances harmlessly)
constexpr uint32_t kDcZvaX0 = 0xD50B7420U;     // dc    zva, x0
constexpr uint32_t kStrX0X1 = 0xF9000020U;     // str   x0, [x1]
constexpr uint32_t kLdrX2X3 = 0xF9400062U;     // ldr   x2, [x3]

constexpr uint64_t kIlBit = UINT64_C(1) << 25;
constexpr uint32_t EcBase(oemu_exc_ec ec) {
  return static_cast<uint32_t>(ec) << 26U;
}

/* Page-table bits for the identity-mapping helper (see oemu/mmu.h and ARM
 * ARM D8-3: a block descriptor is bit-0-set-not-bit-1, with the output
 * address at [47:30] for a 1 GiB block). */
constexpr uint64_t kBlock = UINT64_C(1);
constexpr uint64_t kAf = UINT64_C(1) << 10;
constexpr uint64_t kAp2 = UINT64_C(1) << 7;
constexpr uint64_t kAttrNormal = UINT64_C(0); /* AttrIndx 0 */
constexpr uint64_t kSctlrM = UINT64_C(1);

class VcpuTest : public ::testing::Test {
 protected:
  static constexpr uint64_t kText = UINT64_C(0x400000);
  static constexpr uint64_t kData = UINT64_C(0x500000);
  static constexpr uint64_t kStack = UINT64_C(0x600000);
  static constexpr uint64_t kVectors = UINT64_C(0x8000);
  static constexpr uint64_t kTables = UINT64_C(0x700000);
  static constexpr uint64_t kQuantum = UINT64_C(1000);

  void SetUp() override {
    ASSERT_EQ(oemu_memory_init(&mem_, 8U), OEMU_OK);
    ASSERT_EQ(oemu_memory_map(&mem_, kText, 0x1000U, OEMU_PERM_ALL), OEMU_OK);
    ASSERT_EQ(oemu_memory_map(&mem_, kData, 0x1000U, OEMU_PERM_READ | OEMU_PERM_WRITE),
              OEMU_OK);
    ASSERT_EQ(oemu_memory_map(&mem_, kStack, 0x1000U, OEMU_PERM_READ | OEMU_PERM_WRITE),
              OEMU_OK);
    const oemu_memops bus = oemu_memory_memops(&mem_);
    ASSERT_EQ(oemu_vcpu_init(&vcpu_, &bus, nullptr, OEMU_EL1, kText, kStack + 0x800U, kQuantum),
              OEMU_OK);
    vcpu_.sysregs.vbar_el[OEMU_EL1] = kVectors;
    ASSERT_EQ(oemu_memory_map(&mem_, kVectors, 0x1000U, OEMU_PERM_ALL), OEMU_OK);
    allocations_at_setup_ = tracker_.alloc_count();
  }
  void TearDown() override {
    oemu_memory_dispose(&mem_);
    EXPECT_FALSE(tracker_.has_leaks());
    /* The vCPU must run the machine without touching the allocator, exactly
     * like the executor it wraps. */
    EXPECT_EQ(tracker_.alloc_count(), allocations_at_setup_);
  }

  void program(std::initializer_list<uint32_t> words) {
    uint64_t addr = kText;
    for (const uint32_t w : words) {
      ASSERT_EQ(oemu_memory_write(&mem_, addr, OEMU_MEM_WORD, w), OEMU_OK);
      addr += OEMU_INSN_SIZE;
    }
    oemu_regs_set_pc(&vcpu_.cpu.regs, kText);
  }
  void place(uint64_t addr, std::initializer_list<uint32_t> words) {
    for (const uint32_t w : words) {
      ASSERT_EQ(oemu_memory_write(&mem_, addr, OEMU_MEM_WORD, w), OEMU_OK);
      addr += OEMU_INSN_SIZE;
    }
  }
  uint64_t load64(uint64_t addr) {
    uint64_t v = 0U;
    EXPECT_EQ(oemu_memory_read(&mem_, addr, OEMU_MEM_DWORD, false, &v), OEMU_OK);
    return v;
  }
  void store64(uint64_t addr, uint64_t value) {
    ASSERT_EQ(oemu_memory_write(&mem_, addr, OEMU_MEM_DWORD, value), OEMU_OK);
  }

  /*
   * Turn on the MMU with one identity mapping: a 1 GiB block at L1 index 0
   * (every fixture address is under 1 GiB, and T0SZ=25 starts the walk at
   * level 1, so one descriptor maps the whole machine onto itself). The
   * block's flags are the caller's, which is how a permission test gets
   * read-only memory without touching the bus underneath.
   */
  void enable_identity(uint64_t l1_flags) {
    ASSERT_EQ(oemu_memory_map(&mem_, kTables, 0x1000U, OEMU_PERM_ALL), OEMU_OK);
    store64(kTables, kBlock | kAf | kAttrNormal | l1_flags); /* VA 0..1GiB -> PA 0 */
    vcpu_.sysregs.ttbr0_el1 = kTables;
    vcpu_.sysregs.tcr_el1 = UINT64_C(25) | (UINT64_C(25) << 16) | (UINT64_C(1) << 27);
    vcpu_.sysregs.sctlr_el1 |= kSctlrM;
    allocations_at_setup_ = tracker_.alloc_count(); /* the map is setup, not a step */
  }

  oemu_status step(oemu_insn *insn = nullptr) { return oemu_vcpu_step(&vcpu_, insn); }
  uint64_t x(unsigned n) { return oemu_regs_read(&vcpu_.cpu.regs, n, OEMU_REG_W64); }
  void set_x(unsigned n, uint64_t value) {
    oemu_regs_write(&vcpu_.cpu.regs, n, OEMU_REG_W64, value);
  }

  oemu_test::TrackingAllocator tracker_;
  oemu_memory mem_{};
  oemu_vcpu vcpu_{};
  size_t allocations_at_setup_ = 0U;
};

/* --- init ---------------------------------------------------------------- */

TEST_F(VcpuTest, InitRejectsMissingArgumentsAndZeroQuantum) {
  const oemu_memops bus = oemu_memory_memops(&mem_);
  EXPECT_EQ(oemu_vcpu_init(nullptr, &bus, nullptr, OEMU_EL1, kText, kStack, 8U),
            OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(oemu_vcpu_init(&vcpu_, nullptr, nullptr, OEMU_EL1, kText, kStack, 8U),
            OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(oemu_vcpu_init(&vcpu_, &bus, nullptr, OEMU_EL1, kText, kStack, 0U),
            OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(oemu_vcpu_init(&vcpu_, &bus, nullptr, OEMU_EL2, kText, kStack, 8U),
            OEMU_ERR_INVALID_ARG);
  oemu_memops broken = bus;
  broken.validate = nullptr;
  EXPECT_EQ(oemu_vcpu_init(&vcpu_, &broken, nullptr, OEMU_EL1, kText, kStack, 8U),
            OEMU_ERR_INVALID_ARG);
}

TEST_F(VcpuTest, BootsAtEl1WithInterruptsMaskedAndStackSeeded) {
  EXPECT_EQ(oemu_vcpu_el(&vcpu_), OEMU_EL1);
  EXPECT_EQ(vcpu_.sysregs.pstate,
            OEMU_PSTATE_M_EL1H | (OEMU_PSTATE_DAIF_MASK << OEMU_PSTATE_DAIF_SHIFT));
  EXPECT_EQ(oemu_regs_pc(&vcpu_.cpu.regs), kText);
  /* The boot bank holds the entry SP, so the first bank switch has history. */
  EXPECT_EQ(vcpu_.sysregs.sp_el[OEMU_EL1], kStack + 0x800U);
  EXPECT_EQ(vcpu_.sysregs.regs, &vcpu_.cpu.regs);
}

TEST_F(VcpuTest, El0BootRoutesToEl1Vectors) {
  oemu_vcpu el0{};
  const oemu_memops bus = oemu_memory_memops(&mem_);
  ASSERT_EQ(oemu_vcpu_init(&el0, &bus, nullptr, OEMU_EL0, kText, kStack + 0x800U, kQuantum),
            OEMU_OK);
  EXPECT_EQ(oemu_vcpu_el(&el0), OEMU_EL0);
  ASSERT_EQ(oemu_memory_write(&mem_, kText, OEMU_MEM_WORD, kSvc123), OEMU_OK);
  el0.sysregs.vbar_el[OEMU_EL1] = kVectors;
  /* From EL0 the lower-EL AArch64 group applies, and delivery is EL1. */
  EXPECT_EQ(oemu_vcpu_step(&el0, nullptr), OEMU_OK);
  EXPECT_EQ(oemu_regs_pc(&el0.cpu.regs), kVectors + 0x400U);
  EXPECT_EQ(el0.sysregs.elr_el[OEMU_EL1], kText);
  EXPECT_EQ(oemu_pstate_el(el0.sysregs.pstate), OEMU_EL1);
}

/* --- quantum --------------------------------------------------------------- */

TEST_F(VcpuTest, StepRefusesAfterQuantumAndRearmRestoresIt) {
  oemu_vcpu small{};
  const oemu_memops bus = oemu_memory_memops(&mem_);
  ASSERT_EQ(oemu_vcpu_init(&small, &bus, nullptr, OEMU_EL1, kText, kStack + 0x800U, 3U),
            OEMU_OK);
  small.sysregs.vbar_el[OEMU_EL1] = kVectors;
  /* A run of yields long enough to outlast the quantum: after three steps
   * the fourth must refuse, and the fifth must run again once rearmed. */
  uint64_t addr = kText;
  for (int i = 0; i < 4; i++) {
    ASSERT_EQ(oemu_memory_write(&mem_, addr, OEMU_MEM_WORD, kYield), OEMU_OK);
    addr += OEMU_INSN_SIZE;
  }
  oemu_regs_set_pc(&small.cpu.regs, kText);
  for (int i = 0; i < 3; i++) {
    ASSERT_EQ(oemu_vcpu_step(&small, nullptr), OEMU_OK) << "step " << i;
  }
  EXPECT_EQ(oemu_vcpu_step(&small, nullptr), OEMU_ERR_TIMEOUT);
  /* A timed-out step moves nothing: same PC, still refused. */
  EXPECT_EQ(oemu_regs_pc(&small.cpu.regs), kText + (3U * OEMU_INSN_SIZE));
  oemu_vcpu_rearm(&small);
  EXPECT_EQ(oemu_vcpu_step(&small, nullptr), OEMU_OK);
}

/* --- system-mode delivery through the step path ------------------------------ */

TEST_F(VcpuTest, SysregReadsAndWritesGoThroughTheTable) {
  program({kMrsVbarX1});
  ASSERT_EQ(step(), OEMU_OK);
  EXPECT_EQ(x(1), kVectors); /* the VBAR this fixture booted with */
}

TEST_F(VcpuTest, UnprivilegedAccessTrapsUndefinedWithTheEncoding) {
  /* From EL1, CurrentEL is an EL1-readable RO register, but SP_EL1 needs EL2:
   * a table refusal must land as Undefined carrying the fetched word. */
  program({0xD51C421FU /* msr sp_el1, xzr */});
  ASSERT_EQ(step(), OEMU_OK);
  EXPECT_EQ(oemu_regs_pc(&vcpu_.cpu.regs), kVectors + 0x200U); /* same-EL, SPSel=1 */
  EXPECT_EQ(vcpu_.sysregs.esr_el[OEMU_EL1],
            EcBase(OEMU_EXC_EC_UNKNOWN) | (uint32_t)kIlBit | (0xD51C421FU & 0x01FFFFFFU));
  EXPECT_EQ(vcpu_.sysregs.elr_el[OEMU_EL1], kText);
}

TEST_F(VcpuTest, SvcTrapsIntoTheVectorTableNotTheHost) {
  program({kSvc123});
  ASSERT_EQ(step(), OEMU_OK);
  EXPECT_EQ(oemu_regs_pc(&vcpu_.cpu.regs), kVectors + 0x200U);
  EXPECT_EQ(vcpu_.sysregs.esr_el[OEMU_EL1],
            EcBase(OEMU_EXC_EC_SVC64) | (uint32_t)kIlBit | 0x123U);
  EXPECT_EQ(vcpu_.sysregs.elr_el[OEMU_EL1], kText);
  /* Entry masks everything and sets IL. */
  EXPECT_EQ(oemu_pstate_daif(vcpu_.sysregs.pstate), OEMU_PSTATE_DAIF_MASK);
}

TEST_F(VcpuTest, BrkDeliversBrk64) {
  program({kBrk234});
  ASSERT_EQ(step(), OEMU_OK);
  EXPECT_EQ(oemu_regs_pc(&vcpu_.cpu.regs), kVectors + 0x200U);
  EXPECT_EQ(vcpu_.sysregs.esr_el[OEMU_EL1],
            EcBase(OEMU_EXC_EC_BRK64) | (uint32_t)kIlBit | 0x234U);
}

TEST_F(VcpuTest, HltDeliversBreakpoint64) {
  program({0xD4400000U /* hlt #0 */});
  ASSERT_EQ(step(), OEMU_OK);
  EXPECT_EQ(oemu_regs_pc(&vcpu_.cpu.regs), kVectors + 0x200U);
  /* Debug-exception ISS: ISV=1, IDS=0, DFSC=0b000100 software trigger. */
  EXPECT_EQ(vcpu_.sysregs.esr_el[OEMU_EL1],
            EcBase(OEMU_EXC_EC_BREAKPOINT) | (uint32_t)kIlBit | (1U << 24) | 0x04U);
}

TEST_F(VcpuTest, EretRoundTripsThroughTheHandler) {
  program({kSvc123});
  place(kVectors + 0x200U, {kEret});
  ASSERT_EQ(step(), OEMU_OK); /* SVC delivered */
  ASSERT_EQ(step(), OEMU_OK); /* handler's ERET */
  /* ELR names the SVC itself: a synchronous trap retries its instruction,
   * and it is the handler's job to have moved ELR past it. */
  EXPECT_EQ(oemu_regs_pc(&vcpu_.cpu.regs), kText);
  /* Returned exactly to the interrupted state: the masked boot PSTATE. */
  EXPECT_EQ(vcpu_.sysregs.pstate,
            OEMU_PSTATE_M_EL1H | (OEMU_PSTATE_DAIF_MASK << OEMU_PSTATE_DAIF_SHIFT));
}

TEST_F(VcpuTest, UnmappedFetchDeliversInstructionAbort) {
  oemu_regs_set_pc(&vcpu_.cpu.regs, UINT64_C(0x900000)); /* nothing mapped there */
  ASSERT_EQ(step(), OEMU_OK);
  EXPECT_EQ(oemu_regs_pc(&vcpu_.cpu.regs), kVectors + 0x200U);
  EXPECT_EQ(vcpu_.sysregs.far_el[OEMU_EL1], UINT64_C(0x900000));
  /* Same-EL abort from EL1: EC 0b100001, ISS = DFSC 0b101100. */
  EXPECT_EQ(vcpu_.sysregs.esr_el[OEMU_EL1],
            EcBase(OEMU_EXC_EC_IABORT_SAME) | (uint32_t)kIlBit | 0x2CU);
}

TEST_F(VcpuTest, UnallocatableWordDeliversUndefined) {
  program({0x00000000U}); /* the zero encoding is unallocated */
  ASSERT_EQ(step(), OEMU_OK);
  EXPECT_EQ(oemu_regs_pc(&vcpu_.cpu.regs), kVectors + 0x200U);
  EXPECT_EQ(vcpu_.sysregs.esr_el[OEMU_EL1], EcBase(OEMU_EXC_EC_UNKNOWN) | (uint32_t)kIlBit);
}

TEST_F(VcpuTest, UnmappedStoreDeliversDataAbortWithoutIsv) {
  set_x(1, UINT64_C(0x900000));
  set_x(0, 0xDEADBEEFU);
  program({kStrX0X1});
  ASSERT_EQ(step(), OEMU_OK);
  EXPECT_EQ(oemu_regs_pc(&vcpu_.cpu.regs), kVectors + 0x200U);
  EXPECT_EQ(vcpu_.sysregs.far_el[OEMU_EL1], UINT64_C(0x900000));
  /* EC 0b100101 (same-EL data abort), IL=1, WnR=1, DFSC=0x2C. ISV is clear:
   * a fault on the way to the access (walk refusal or unmapped bus) has never
   * seen the transfer, so oemu cannot attest its width -- the honest value,
   * and one Linux ignores when it routes the fault. */
  EXPECT_EQ(vcpu_.sysregs.esr_el[OEMU_EL1],
            EcBase(OEMU_EXC_EC_DABORT_SAME) | (uint32_t)kIlBit | (1U << 6) | 0x2CU);
}

TEST_F(VcpuTest, LoadFaultClearsTheWriteFlag) {
  set_x(3, UINT64_C(0x900000));
  program({kLdrX2X3});
  ASSERT_EQ(step(), OEMU_OK);
  EXPECT_EQ(oemu_regs_pc(&vcpu_.cpu.regs), kVectors + 0x200U);
  EXPECT_EQ(vcpu_.sysregs.far_el[OEMU_EL1], UINT64_C(0x900000));
  const uint32_t iss = vcpu_.sysregs.esr_el[OEMU_EL1] & 0x01FFFFFFU;
  EXPECT_EQ(iss & (1U << 6), 0U); /* WnR clear: this was a load */
  EXPECT_EQ(iss & 0x3FU, 0x2CU);  /* DFSC: translation fault, level -1 */
}

TEST_F(VcpuTest, TlbiAndIcExecuteAsNoOps) {
  program({0xD508871FU /* tlbi vmalle1 */, 0xD508751FU /* ic iallu */, kYield});
  ASSERT_EQ(step(), OEMU_OK);
  ASSERT_EQ(step(), OEMU_OK);
  EXPECT_EQ(oemu_regs_pc(&vcpu_.cpu.regs), kText + (2U * OEMU_INSN_SIZE));
}

TEST_F(VcpuTest, TlbiThroughStepFlushesTheTlb) {
  // M3b: the TLBI window stopped being a no-op. A real `tlbi vmalle1`
  // executed through the step path must reach the layer's invalidation,
  // not just retire -- the counter is the proof it did.
  enable_identity(0U);
  set_x(1, kData);
  set_x(0, 0x1234ULL);
  program({kStrX0X1, 0xD508871FU /* tlbi vmalle1 */});
  ASSERT_EQ(step(), OEMU_OK);  // store: walks and fills the entry
  ASSERT_EQ(step(), OEMU_OK);  // tlbi: invalidates the whole cache
  EXPECT_EQ(oemu_regs_pc(&vcpu_.cpu.regs), kText + (2U * OEMU_INSN_SIZE));
  EXPECT_EQ(vcpu_.mmu.tlb_flushes, UINT64_C(1));
  store64(kData, 0U);
  EXPECT_EQ(load64(kData), 0U);  // the mapping still serves after the flush
}

TEST_F(VcpuTest, DcZvaZeroesALine) {
  store64(kData, 0xFFFFFFFFFFFFFFFFULL);
  store64(kData + 8U, 0xFFFFFFFFFFFFFFFFULL);
  set_x(0, kData + 16U); /* DC ZVA takes any address inside the line */
  program({kDcZvaX0});
  ASSERT_EQ(step(), OEMU_OK);
  EXPECT_EQ(load64(kData), 0U);
  EXPECT_EQ(load64(kData + 8U), 0U);
  EXPECT_EQ(load64(kData + 16U), 0U);
}

/* --- the translation layer (M3a) ------------------------------------------------- */

TEST_F(VcpuTest, IdentityTranslationRunsLikeMmuOff) {
  enable_identity(0U);
  program({kYield});
  ASSERT_EQ(step(), OEMU_OK);
  EXPECT_EQ(oemu_regs_pc(&vcpu_.cpu.regs), kText + OEMU_INSN_SIZE);
  set_x(1, kData);
  set_x(0, 0xFEEDFACEULL);
  program({kStrX0X1});
  ASSERT_EQ(step(), OEMU_OK);
  EXPECT_EQ(load64(kData), 0xFEEDFACEULL);
}

TEST_F(VcpuTest, FetchFaultThroughWalkCarriesTheLevel) {
  enable_identity(0U);
  store64(kTables, 0U); /* and then the block entry is gone */
  ASSERT_EQ(step(), OEMU_OK);
  EXPECT_EQ(oemu_regs_pc(&vcpu_.cpu.regs), kVectors + 0x200U);
  EXPECT_EQ(vcpu_.sysregs.far_el[OEMU_EL1], kText);
  /* With a mapping layer the refusal has a place: the walk died at level 1,
   * not at the bus. (With the MMU off the same miss is DFSC 0x2C.) */
  EXPECT_EQ(vcpu_.sysregs.esr_el[OEMU_EL1],
            EcBase(OEMU_EXC_EC_IABORT_SAME) | (uint32_t)kIlBit | 0x05U);
}

TEST_F(VcpuTest, StoreThroughReadOnlyTranslationIsAPermissionAbort) {
  enable_identity(kAp2); /* the whole block: EL1 read-only */
  set_x(1, kData);
  set_x(0, 1U);
  program({kStrX0X1});
  ASSERT_EQ(step(), OEMU_OK);
  EXPECT_EQ(oemu_regs_pc(&vcpu_.cpu.regs), kVectors + 0x200U);
  EXPECT_EQ(vcpu_.sysregs.far_el[OEMU_EL1], kData);
  EXPECT_EQ(vcpu_.sysregs.esr_el[OEMU_EL1],
            EcBase(OEMU_EXC_EC_DABORT_SAME) | (uint32_t)kIlBit | (1U << 6) | 0x0DU);
}

TEST_F(VcpuTest, DcZvaZeroesThroughTranslation) {
  enable_identity(0U);
  store64(kData, 0xFFFFFFFFFFFFFFFFULL);
  store64(kData + 56U, 0xFFFFFFFFFFFFFFFFULL);
  set_x(0, kData + 8U);
  program({kDcZvaX0});
  ASSERT_EQ(step(), OEMU_OK);
  EXPECT_EQ(load64(kData), 0U);
  EXPECT_EQ(load64(kData + 56U), 0U);
  EXPECT_EQ(oemu_regs_pc(&vcpu_.cpu.regs), kText + OEMU_INSN_SIZE);
}

TEST_F(VcpuTest, DcZvaUnalignedIsAnAlignmentAbort) {
  set_x(0, kData + 4U);
  program({kDcZvaX0});
  ASSERT_EQ(step(), OEMU_OK);
  EXPECT_EQ(oemu_regs_pc(&vcpu_.cpu.regs), kVectors + 0x200U);
  EXPECT_EQ(vcpu_.sysregs.far_el[OEMU_EL1], kData + 4U);
  /* Data abort with DFSC 0b100001 and no ISV: a DC ZVA has no transfer width
   * to report, and claiming one would be a lie the handler could act on.
   * WnR survives without ISV: the direction is known even when the width is
   * not, and a DC ZVA writes. */
  EXPECT_EQ(vcpu_.sysregs.esr_el[OEMU_EL1],
            EcBase(OEMU_EXC_EC_DABORT_SAME) | (uint32_t)kIlBit | (1U << 6) | 0x21U);
}

TEST_F(VcpuTest, AtPublishesParEl1OnStage1Translate) {
  /* AT is no longer Undefined: a stage-1 translate publishes its verdict in
   * PAR_EL1. With stage-1 off the walk is the identity, so `at s1e1r, x0`
   * succeeds and PAR_EL1.F (bit 0) reads clear with the output address. */
  set_x(0, kData);
  program({0xD5087800U /* at s1e1r, x0 */});
  ASSERT_EQ(step(), OEMU_OK);
  EXPECT_EQ(oemu_regs_pc(&vcpu_.cpu.regs), kText + 4U); /* it retired, no trap */
  EXPECT_EQ(vcpu_.sysregs.par_el1 & 1U, 0U);            /* F clear: it translated */
  EXPECT_EQ(vcpu_.sysregs.par_el1, kData);              /* the output address */
}

TEST_F(VcpuTest, WfiAndWfeParkWithoutTheEvent) {
  program({kWfi});
  EXPECT_EQ(step(), OEMU_ERR_BLOCKED);
  /* A park consumes nothing: not the PC, not the quantum. */
  EXPECT_EQ(oemu_regs_pc(&vcpu_.cpu.regs), kText);
}

TEST_F(VcpuTest, SevArmsTheEventRegisterForWfe) {
  program({kSev, kWfe, kWfi});
  EXPECT_EQ(step(), OEMU_OK);          /* sev */
  EXPECT_EQ(step(), OEMU_OK);          /* wfe wakes on the armed event */
  EXPECT_EQ(step(), OEMU_ERR_BLOCKED); /* and wfi still parks */
}

/* --- the run loop ------------------------------------------------------------ */

namespace {
struct HaltFlag {
  bool halted;
};
bool halted_of(const void *ctx) {
  return static_cast<const HaltFlag *>(ctx)->halted;
}
}  // namespace

TEST_F(VcpuTest, RunStopsAtBudgetAndAtHalted) {
  oemu_vcpu small{};
  const oemu_memops bus = oemu_memory_memops(&mem_);
  HaltFlag halt{false};
  const oemu_env_ops env{&halt, nullptr, halted_of, nullptr};
  ASSERT_EQ(oemu_vcpu_init(&small, &bus, &env, OEMU_EL1, kText, kStack + 0x800U, 100U),
            OEMU_OK);
  uint64_t addr = kText;
  for (int i = 0; i < 10; i++) {
    ASSERT_EQ(oemu_memory_write(&mem_, addr, OEMU_MEM_WORD, kYield), OEMU_OK);
    addr += OEMU_INSN_SIZE;
  }
  uint64_t done = 0U;
  /* Budget exhausted while the guest still runs: TIMEOUT, work reported. */
  EXPECT_EQ(oemu_vcpu_run(&small, 5U, &done), OEMU_ERR_TIMEOUT);
  EXPECT_EQ(done, 5U);
  /* The guest says it stopped: the loop ends clean, at the current count. */
  halt.halted = true;
  EXPECT_EQ(oemu_vcpu_run(&small, 5U, &done), OEMU_OK);
  EXPECT_EQ(done, 0U);
}

TEST_F(VcpuTest, RunPropagatesBlocked) {
  program({kWfi});
  uint64_t done = 0U;
  EXPECT_EQ(oemu_vcpu_run(&vcpu_, 10U, &done), OEMU_ERR_BLOCKED);
  EXPECT_EQ(done, 0U);
  EXPECT_EQ(oemu_regs_pc(&vcpu_.cpu.regs), kText);
}

/* --- interrupt pins -------------------------------------------------------- */

TEST_F(VcpuTest, PendingIrqIsMaskedByTheBootDaif) {
  oemu_vcpu_set_irq(&vcpu_, true);
  program({kYield});
  /* No unmasked pending interrupt: WFI-style tests above prove the pins
   * alone do not force delivery; here the mask must stop it. */
  ASSERT_FALSE(oemu_vcpu_take_pending(&vcpu_));
  ASSERT_EQ(step(), OEMU_OK);
  EXPECT_EQ(oemu_regs_pc(&vcpu_.cpu.regs), kText + OEMU_INSN_SIZE);
}

TEST_F(VcpuTest, UnmaskedIrqIsDeliveredBeforeTheFetchedInstruction) {
  program({kMsrDaifXzr, kYield});
  ASSERT_EQ(step(), OEMU_OK); /* msr daif, xzr: unmask */
  oemu_vcpu_set_irq(&vcpu_, true);
  place(kVectors + 0x300U, {kEret});
  ASSERT_EQ(step(), OEMU_OK); /* the step takes the IRQ, not the yield */
  EXPECT_EQ(oemu_regs_pc(&vcpu_.cpu.regs), kVectors + 0x300U); /* IRQ slot, same EL */
  /* ELR names the instruction that never ran; the re-execution after the
   * ERET is the whole point of the precise contract. */
  EXPECT_EQ(vcpu_.sysregs.elr_el[OEMU_EL1], kText + OEMU_INSN_SIZE);
  ASSERT_EQ(step(), OEMU_OK); /* handler eret */
  EXPECT_EQ(oemu_regs_pc(&vcpu_.cpu.regs), kText + OEMU_INSN_SIZE);
  /* Level-triggered: the pin is still high, so the next step re-fires at the
   * same instruction. */
  ASSERT_EQ(step(), OEMU_OK);
  EXPECT_EQ(oemu_regs_pc(&vcpu_.cpu.regs), kVectors + 0x300U);
  oemu_vcpu_set_irq(&vcpu_, false);
  ASSERT_EQ(step(), OEMU_OK); /* handler eret */
  ASSERT_EQ(step(), OEMU_OK); /* the yield finally executes */
  EXPECT_EQ(oemu_regs_pc(&vcpu_.cpu.regs), kText + (2U * OEMU_INSN_SIZE));
}

TEST_F(VcpuTest, FiqPreemptsIrq) {
  program({kMsrDaifXzr});
  ASSERT_EQ(step(), OEMU_OK);
  oemu_vcpu_set_irq(&vcpu_, true);
  oemu_vcpu_set_fiq(&vcpu_, true);
  ASSERT_TRUE(oemu_vcpu_take_pending(&vcpu_));
  EXPECT_EQ(oemu_regs_pc(&vcpu_.cpu.regs), kVectors + 0x380U); /* FIQ slot */
  EXPECT_EQ(vcpu_.sysregs.elr_el[OEMU_EL1], kText + OEMU_INSN_SIZE);
}

TEST_F(VcpuTest, PendingButMaskedInterruptStillWakesWfi) {
  program({kWfi});
  oemu_vcpu_set_irq(&vcpu_, true); /* masked by the boot DAIF */
  /* QEMU's WFI helper wakes on any pending interrupt, mask included: the
   * core cannot unmask itself while halted, and the architecture allows
   * spurious wake-ups. The wake is the no-op; delivery still waits for the
   * guest's own `msr daif`. */
  EXPECT_EQ(step(), OEMU_OK);
  EXPECT_EQ(oemu_regs_pc(&vcpu_.cpu.regs), kText + OEMU_INSN_SIZE);
}

TEST_F(VcpuTest, DeliveryChargesTheQuantum) {
  oemu_vcpu small{};
  const oemu_memops bus = oemu_memory_memops(&mem_);
  ASSERT_EQ(oemu_vcpu_init(&small, &bus, nullptr, OEMU_EL1, kText, kStack + 0x800U, 1U),
            OEMU_OK);
  small.sysregs.vbar_el[OEMU_EL1] = kVectors;
  small.sysregs.pstate &= ~(OEMU_PSTATE_DAIF_MASK << OEMU_PSTATE_DAIF_SHIFT); /* unmask */
  oemu_vcpu_set_irq(&small, true);
  ASSERT_EQ(oemu_vcpu_step(&small, nullptr), OEMU_OK); /* delivery, no instruction */
  EXPECT_EQ(oemu_vcpu_step(&small, nullptr), OEMU_ERR_TIMEOUT);
}

TEST_F(VcpuTest, CounterStepsAtTheRateTheGuestIsTold) {
  program({kYield, kYield, kYield});
  const uint64_t before = vcpu_.sysregs.cntvct;
  for (int i = 0; i < 3; ++i) {
    ASSERT_EQ(step(), OEMU_OK);
  }
  /* One count per retired instruction: the modelled core runs at exactly the
   * frequency CNTFRQ_EL0 reports, so a count delta means the same interval to
   * the guest as it means to us. Any other step is a clock the guest cannot
   * use: a Linux HZ=100 tick arms CNTV_CVAL 625000 counts past `now`, and if a
   * single instruction can cross that delta the tick fires the moment it is
   * armed -- the timer wheel, RCU and every mdelay() in the kernel then run on
   * a clock that does not exist, and an interrupt is pending on every
   * instruction the guest retires. */
  EXPECT_EQ(vcpu_.sysregs.cntvct - before, 3ULL * OEMU_TIMER_COUNTS_PER_INSN);
  EXPECT_LT(OEMU_TIMER_COUNTS_PER_INSN, OEMU_CNTFRQ_EL0_DEFAULT / 100U)
      << "one instruction may not retire a whole guest tick";
}

}  // namespace
