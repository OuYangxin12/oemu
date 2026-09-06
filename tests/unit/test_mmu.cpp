/*
 * Tests for the translation layer over a real address space: hand-built
 * page tables in RAM, every fault class from the walk, banding, top-byte
 * ignore, the alignment policy, and the bus wrappers' split/validate rules.
 *
 * Page tables are written through the aspace before each access, the way
 * boot code does it: the tables live in the single identity-mapped RAM
 * region until the mapping machinery itself is the thing under test.
 *
 * With TCR.T0SZ=25 the walk starts at level 1, so the index fields are
 *   L1: bits [38:30], L2: bits [29:21], L3: bits [20:12]
 * -- and RAM's bit 30 makes L1 index 1 for every kRam-based address.
 */
#include "oemu/aspace.h"
#include "oemu/exc.h"
#include "oemu/exec.h"
#include "oemu/mmu.h"
#include "oemu/regs.h"
#include "oemu/sysreg.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <gtest/gtest.h>

namespace {

/* One 16 MiB RAM region at the QEMU virt RAM base, and an unmapped hole at
 * PA 0 whose every access is the bus's own refusal. */
constexpr uint64_t kRam = UINT64_C(0x40000000);
constexpr uint64_t kRamSize = UINT64_C(0x1000000);

/* The page tables, in RAM, identity-mapped (the walk is over physical
 * addresses, so with a flat map the table VAs are the table PAs). */
constexpr uint64_t kL1 = kRam + 0x100000U;  /* TTBR0: the lower-region start table */
constexpr uint64_t kL1b = kRam + 0x101000U; /* TTBR1: the upper-region start table */
constexpr uint64_t kL2 = kRam + 0x102000U;  /* L2 under L1 idx 1 */
constexpr uint64_t kL3 = kRam + 0x103000U;  /* L3 under L2 idx 1 */

/* Descriptor bits. */
constexpr uint64_t kTable = UINT64_C(3); /* table descriptors are type 0b11 */
constexpr uint64_t kBlock = UINT64_C(1);
constexpr uint64_t kAf = UINT64_C(1) << 10;
constexpr uint64_t kAp1 = UINT64_C(1) << 6;
constexpr uint64_t kAp2 = UINT64_C(1) << 7;
constexpr uint64_t kPxn = UINT64_C(1) << 53;
constexpr uint64_t kXn = UINT64_C(1) << 54;
constexpr uint64_t kAttrNormal = UINT64_C(0) << 2; /* AttrIndx 0: the MAIR entry is
                                                    * parsed and ignored */
/* Table descriptor constraints. */
constexpr uint64_t kTPxn = UINT64_C(1) << 59;
constexpr uint64_t kTXn = UINT64_C(1) << 60;
constexpr uint64_t kTApt0 = UINT64_C(1) << 61;
constexpr uint64_t kTApt1 = UINT64_C(1) << 62;

constexpr uint64_t kSctlrM = UINT64_C(1) << 0;
constexpr uint64_t kSctlrA = UINT64_C(1) << 1;
constexpr uint64_t kSctlrSa0 = UINT64_C(1) << 4;
constexpr uint64_t kTcrPs36 = UINT64_C(1) << 27;
constexpr uint64_t kTcrEpd0 = UINT64_C(1) << 7;
constexpr uint64_t kTcrTbi0 = UINT64_C(1) << 24;

constexpr uint32_t kIlBit = UINT32_C(1) << 25;
constexpr uint32_t kWnr = UINT32_C(1) << 6;

constexpr uint32_t EcBase(uint32_t ec) {
  return ec << 26U;
}

/* DFSC shorthand, pinned against Linux arch/arm64/include/asm/esr.h. */
constexpr uint32_t Trans(int level) {
  return 0x04U + static_cast<uint32_t>(level);
}
constexpr uint32_t AccessFlag(int level) {
  return 0x08U + static_cast<uint32_t>(level);
}
constexpr uint32_t Perm(int level) {
  return 0x0CU + static_cast<uint32_t>(level);
}
constexpr uint32_t AddrSize(int level) {
  return (level < 0) ? 0x29U : (static_cast<uint32_t>(level) + 0x00U);
}
constexpr uint32_t kNoWalk = 0x2CU;
constexpr uint32_t kAlign = 0x21U;

class MmuTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(oemu_aspace_init(&as_, 4U), OEMU_OK);
    void *ram = nullptr;
    ASSERT_EQ(oemu_aspace_map_ram(&as_, kRam, kRamSize, OEMU_PERM_ALL, &ram), OEMU_OK);
    /* A powered-off machine's RAM is not malloc garbage: an invalid
     * descriptor is all zeroes, and every test here starts from blank
     * tables. */
    memset(ram, 0, static_cast<size_t>(kRamSize));
    ASSERT_EQ(oemu_cpu_init(&cpu_, kRam, kRam + 0x8000U), OEMU_OK);
    oemu_sysregs_init(&sr_, &cpu_.regs, OEMU_EL1);
    el1();
    const oemu_memops bus = oemu_aspace_memops(&as_);
    oemu_mmu_init(&mmu_, &sr_, &bus);
    view_ = oemu_mmu_memops(&mmu_);
    /* Geometry: 39-bit VA (start level 1), 36-bit PA (the advertised PARANGE). */
    sr_.tcr_el1 = 25U | (UINT64_C(25) << 16) | kTcrPs36;
    sr_.ttbr0_el1 = kL1;
    sr_.ttbr1_el1 = kL1b;
    sr_.sctlr_el1 = kSctlrM;
  }
  void TearDown() override { oemu_aspace_dispose(&as_); }

  void write_desc(uint64_t pa, uint64_t desc) {
    ASSERT_EQ(oemu_aspace_write(&as_, pa, OEMU_MEM_DWORD, desc), OEMU_OK);
  }
  void write_u64(uint64_t pa, uint64_t v) {
    ASSERT_EQ(oemu_aspace_write(&as_, pa, OEMU_MEM_DWORD, v), OEMU_OK);
  }
  uint64_t read_u64(uint64_t pa) {
    uint64_t v = 0U;
    EXPECT_EQ(oemu_aspace_read(&as_, pa, OEMU_MEM_DWORD, false, &v), OEMU_OK);
    return v;
  }
  uint32_t pending_esr() {
    oemu_mmu_fault f{};
    EXPECT_TRUE(oemu_mmu_take_fault(&mmu_, &f));
    return f.esr;
  }

  void el1() { sr_.pstate = OEMU_PSTATE_M_EL1H | (0xFULL << 6U); }
  void el0() { sr_.pstate = OEMU_PSTATE_M_EL0T | (0xFULL << 6U); }
  void el3() { sr_.pstate = OEMU_PSTATE_M_EL3H | (0xFULL << 6U); }

  /* Geometry helpers: an index for the virtual address at each level. */
  static unsigned idx1(uint64_t va) { return static_cast<unsigned>((va >> 30U) & 0x1FFU); }
  static unsigned idx2(uint64_t va) { return static_cast<unsigned>((va >> 21U) & 0x1FFU); }
  static unsigned idx3(uint64_t va) { return static_cast<unsigned>((va >> 12U) & 0x1FFU); }

  void l1_block(uint64_t l1, uint64_t va, uint64_t pa, uint64_t flags) {
    write_desc(l1 + idx1(va) * 8U, kBlock | pa | kAf | kAttrNormal | flags);
  }
  void l1_table(uint64_t l1, uint64_t va, uint64_t next, uint64_t flags) {
    write_desc(l1 + idx1(va) * 8U, kTable | next | flags);
  }
  void l2_table(uint64_t va, uint64_t next) { write_desc(kL2 + idx2(va) * 8U, kTable | next); }
  void l3_page(uint64_t va, uint64_t pa, uint64_t flags) {
    write_desc(kL3 + idx3(va) * 8U, kBlock | pa | kAf | kAttrNormal | flags);
  }

  oemu_status xlat(uint64_t va, bool is_write, bool is_fetch, uint64_t *pa, oemu_mmu_fault *f) {
    return oemu_mmu_translate(&mmu_, va, is_write, is_fetch, pa, f);
  }

  oemu_aspace as_{};
  oemu_cpu cpu_{};
  oemu_sysregs sr_{};
  oemu_mmu mmu_{};
  oemu_memops view_{};
};

/* --- bypass conditions ------------------------------------------------------ */

TEST_F(MmuTest, MmuDisabledIsIdentity) {
  sr_.sctlr_el1 = 0U;
  uint64_t pa = 0U;
  ASSERT_EQ(xlat(0x1234ULL, false, false, &pa, nullptr), OEMU_OK);
  EXPECT_EQ(pa, 0x1234ULL);
}

TEST_F(MmuTest, El3RunsUntranslated) {
  el3(); /* SCTLR_EL3 is not modelled: the firmware's level is identity */
  uint64_t pa = 0U;
  ASSERT_EQ(xlat(0x1234ULL, false, false, &pa, nullptr), OEMU_OK);
  EXPECT_EQ(pa, 0x1234ULL);
}

/* --- the happy walks ---------------------------------------------------------- */

TEST_F(MmuTest, OneGiBBlockIdentity) {
  l1_block(kL1, kRam, kRam, 0U);
  uint64_t pa = 0U;
  ASSERT_EQ(xlat(kRam + 0x1234ULL, false, false, &pa, nullptr), OEMU_OK);
  EXPECT_EQ(pa, kRam + 0x1234ULL);
}

TEST_F(MmuTest, BlockOffsetComesFromTheVirtualAddress) {
  l1_block(kL1, kRam, kRam, 0U);
  uint64_t pa = 0U;
  ASSERT_EQ(xlat(kRam + UINT64_C(0x3FFF), false, false, &pa, nullptr), OEMU_OK);
  EXPECT_EQ(pa, kRam + UINT64_C(0x3FFF));
}

TEST_F(MmuTest, TwoLevelWalkLandsOnThePage) {
  const uint64_t va = kRam + UINT64_C(0x2000) + 0x44ULL;
  l1_table(kL1, va, kL2, 0U);
  l2_table(va, kL3);
  l3_page(va, kRam + UINT64_C(0x9000), 0U);
  uint64_t pa = 0U;
  ASSERT_EQ(xlat(va, false, false, &pa, nullptr), OEMU_OK);
  EXPECT_EQ(pa, kRam + UINT64_C(0x9000) + 0x44ULL);
}

/* --- walk refusals -------------------------------------------------------------- */

TEST_F(MmuTest, InvalidLevel1EntryIsATranslationFaultAtLevel1) {
  write_desc(kL1 + idx1(kRam) * 8U, 0U);
  uint64_t pa = 0U;
  oemu_mmu_fault f{};
  ASSERT_EQ(xlat(kRam, false, false, &pa, &f), OEMU_ERR_FAULT);
  EXPECT_EQ(f.far, kRam);
  EXPECT_EQ(f.esr, EcBase(OEMU_EXC_EC_DABORT_SAME) | kIlBit | Trans(1));
}

TEST_F(MmuTest, FullWidthGeometryIsATranslationFaultAtLevel0) {
  sr_.tcr_el1 = kTcrPs36; /* T0SZ=0: a 64-bit input the granule cannot walk */
  uint64_t pa = 0U;
  oemu_mmu_fault f{};
  ASSERT_EQ(xlat(0ULL, false, false, &pa, &f), OEMU_ERR_FAULT);
  EXPECT_EQ(f.esr, EcBase(OEMU_EXC_EC_DABORT_SAME) | kIlBit | Trans(0));
}

TEST_F(MmuTest, ReservedDescriptorEncodingIsATranslationFault) {
  write_desc(kL1 + idx1(kRam) * 8U, UINT64_C(2) | kRam); /* valid, table bit, type 10 */
  uint64_t pa = 0U;
  oemu_mmu_fault f{};
  ASSERT_EQ(xlat(kRam, false, false, &pa, &f), OEMU_ERR_FAULT);
  EXPECT_EQ(f.esr, EcBase(OEMU_EXC_EC_DABORT_SAME) | kIlBit | Trans(1));
}

TEST_F(MmuTest, TableDescriptorBelowThePageLevelFaults) {
  const uint64_t va = kRam + UINT64_C(0x2000);
  l1_table(kL1, va, kL2, 0U);
  l2_table(va, kL3);
  write_desc(kL3 + idx3(va) * 8U, kTable | (kRam + 0x200000U)); /* table below pages */
  uint64_t pa = 0U;
  oemu_mmu_fault f{};
  ASSERT_EQ(xlat(va, false, false, &pa, &f), OEMU_ERR_FAULT);
  EXPECT_EQ(f.esr, EcBase(OEMU_EXC_EC_DABORT_SAME) | kIlBit | Trans(3));
}

TEST_F(MmuTest, BlockDescriptorAtLevel0IsRefused) {
  /* Without FEAT_LPA2 a block at level 0 is reserved; T0SZ=16 walks from
   * level 0 and hits it. */
  sr_.tcr_el1 = 16U | (UINT64_C(16) << 16) | kTcrPs36;
  l1_block(kL1, kRam, kRam, 0U);
  uint64_t pa = 0U;
  oemu_mmu_fault f{};
  ASSERT_EQ(xlat(kRam, false, false, &pa, &f), OEMU_ERR_FAULT);
  EXPECT_EQ(f.esr, EcBase(OEMU_EXC_EC_DABORT_SAME) | kIlBit | Trans(0));
}

TEST_F(MmuTest, GapBetweenRegionsIsATranslationFaultAtTheStartLevel) {
  /* 39-bit lower region: an address with bits above bit 38 set but below
   * bit 55 is neither region's sign extension. */
  const uint64_t va = UINT64_C(1) << 40U;
  l1_block(kL1, kRam, kRam, 0U);
  uint64_t pa = 0U;
  oemu_mmu_fault f{};
  ASSERT_EQ(xlat(va, false, false, &pa, &f), OEMU_ERR_FAULT);
  EXPECT_EQ(f.far, va);
  EXPECT_EQ(f.esr, EcBase(OEMU_EXC_EC_DABORT_SAME) | kIlBit | Trans(1));
}

TEST_F(MmuTest, Epd0DisablesTheLowerWalkWithATranslationFault) {
  sr_.tcr_el1 |= kTcrEpd0;
  l1_block(kL1, kRam, kRam, 0U);
  uint64_t pa = 0U;
  oemu_mmu_fault f{};
  ASSERT_EQ(xlat(kRam, false, false, &pa, &f), OEMU_ERR_FAULT);
  EXPECT_EQ(f.esr, EcBase(OEMU_EXC_EC_DABORT_SAME) | kIlBit | Trans(1));
}

TEST_F(MmuTest, TtbrAddressSizeFaultHasNoLevel) {
  sr_.tcr_el1 = 25U | (UINT64_C(25) << 16);  /* PS=0: 32-bit output */
  sr_.ttbr0_el1 = kL1 | (UINT64_C(1) << 32); /* and the table lives above it */
  uint64_t pa = 0U;
  oemu_mmu_fault f{};
  ASSERT_EQ(xlat(kRam, false, false, &pa, &f), OEMU_ERR_FAULT);
  EXPECT_EQ(f.esr, EcBase(OEMU_EXC_EC_DABORT_SAME) | kIlBit | AddrSize(-1));
}

TEST_F(MmuTest, DescriptorAddressSizeFaultAtItsLevel) {
  l1_table(kL1, kRam, kL2, 0U);
  write_desc(kL2 + idx2(kRam) * 8U, kTable | (UINT64_C(1) << 36U)); /* above PS=36 */
  uint64_t pa = 0U;
  oemu_mmu_fault f{};
  ASSERT_EQ(xlat(kRam, false, false, &pa, &f), OEMU_ERR_FAULT);
  EXPECT_EQ(f.esr, EcBase(OEMU_EXC_EC_DABORT_SAME) | kIlBit | AddrSize(2));
}

TEST_F(MmuTest, WalkTablePageUnbackedByBusIsTheNoLevelTranslationFault) {
  sr_.ttbr0_el1 = UINT64_C(0x1000); /* no region exists at PA 0x1000 */
  uint64_t pa = 0U;
  oemu_mmu_fault f{};
  ASSERT_EQ(xlat(kRam, false, false, &pa, &f), OEMU_ERR_FAULT);
  EXPECT_EQ(f.esr, EcBase(OEMU_EXC_EC_DABORT_SAME) | kIlBit | kNoWalk);
}

TEST_F(MmuTest, MappedButUnbackedLeafIsTheNoLevelDataAbort) {
  const uint64_t va = 0x1040ULL; /* lower region, L1 idx 0, L3 idx 1 */
  l1_table(kL1, va, kL2, 0U);
  l2_table(va, kL3);
  l3_page(va, UINT64_C(0x1000), 0U); /* mapped into the hole at PA 0 */
  ASSERT_EQ(view_.write(view_.ctx, va, OEMU_MEM_WORD, 1U), OEMU_ERR_FAULT);
  const uint32_t esr = pending_esr();
  EXPECT_EQ(esr, EcBase(OEMU_EXC_EC_DABORT_SAME) | kIlBit | kWnr | kNoWalk);
}

/* --- access flags --------------------------------------------------------------- */

TEST_F(MmuTest, ClearedAccessFlagIsAnAccessFlagFaultAtTheLeafLevel) {
  write_desc(kL1 + idx1(kRam) * 8U, kBlock | kRam | kAttrNormal); /* no AF */
  uint64_t pa = 0U;
  oemu_mmu_fault f{};
  ASSERT_EQ(xlat(kRam, false, false, &pa, &f), OEMU_ERR_FAULT);
  EXPECT_EQ(f.far, kRam);
  EXPECT_EQ(f.esr, EcBase(OEMU_EXC_EC_DABORT_SAME) | kIlBit | AccessFlag(1));
}

TEST_F(MmuTest, AccessFlagFaultsOnFetchesToo) {
  write_desc(kL1 + idx1(kRam) * 8U, kBlock | kRam); /* no AF */
  uint64_t pa = 0U;
  oemu_mmu_fault f{};
  ASSERT_EQ(xlat(kRam, false, true, &pa, &f), OEMU_ERR_FAULT);
  EXPECT_EQ(f.esr, EcBase(OEMU_EXC_EC_IABORT_SAME) | kIlBit | AccessFlag(1));
}

/* --- permissions --------------------------------------------------------------- */

TEST_F(MmuTest, KernelReadOnlyPageDeniesEl1WriteAndPermitsEl1Read) {
  l1_block(kL1, kRam, kRam, kAp2); /* AP=0b10: EL1 read-only, no EL0 */
  uint64_t pa = 0U;
  ASSERT_EQ(xlat(kRam, false, false, &pa, nullptr), OEMU_OK);
  oemu_mmu_fault f{};
  ASSERT_EQ(xlat(kRam, true, false, &pa, &f), OEMU_ERR_FAULT);
  EXPECT_EQ(f.esr, EcBase(OEMU_EXC_EC_DABORT_SAME) | kIlBit | kWnr | Perm(1));
}

TEST_F(MmuTest, UserPageGatesEl0ByAp) {
  l1_block(kL1, kRam, kRam, 0U); /* AP=0b00: kernel only */
  el0();
  uint64_t pa = 0U;
  oemu_mmu_fault f{};
  ASSERT_EQ(xlat(kRam, false, false, &pa, &f), OEMU_ERR_FAULT);
  EXPECT_EQ(f.esr, EcBase(OEMU_EXC_EC_DABORT_LOWER) | kIlBit | Perm(1));
  l1_block(kL1, kRam, kRam, kAp1); /* AP=0b01 */
  ASSERT_EQ(xlat(kRam, true, false, &pa, nullptr), OEMU_OK);
  l1_block(kL1, kRam, kRam, kAp1 | kAp2); /* AP=0b11: user read-only */
  ASSERT_EQ(xlat(kRam, false, false, &pa, nullptr), OEMU_OK);
  ASSERT_EQ(xlat(kRam, true, false, &pa, &f), OEMU_ERR_FAULT);
  EXPECT_EQ(f.esr, EcBase(OEMU_EXC_EC_DABORT_LOWER) | kIlBit | kWnr | Perm(1));
}

TEST_F(MmuTest, XnAndPxnGateOnlyFetchesOfTheirOwnEl) {
  l1_block(kL1, kRam, kRam, kXn | kAp1);
  uint64_t pa = 0U;
  ASSERT_EQ(xlat(kRam, false, false, &pa, nullptr), OEMU_OK); /* XN is not EL1's gate */
  oemu_mmu_fault f{};
  ASSERT_EQ(xlat(kRam, false, true, &pa, &f), OEMU_OK);
  el0();
  ASSERT_EQ(xlat(kRam, false, true, &pa, &f), OEMU_ERR_FAULT);
  EXPECT_EQ(f.esr, EcBase(OEMU_EXC_EC_IABORT_LOWER) | kIlBit | Perm(1));
  l1_block(kL1, kRam, kRam, kPxn);
  el1();
  ASSERT_EQ(xlat(kRam, false, true, &pa, &f), OEMU_ERR_FAULT);
  EXPECT_EQ(f.esr, EcBase(OEMU_EXC_EC_IABORT_SAME) | kIlBit | Perm(1));
}

TEST_F(MmuTest, TableConstraintsBindEverythingBelowThem) {
  const uint64_t va = kRam + UINT64_C(0x2000);
  l1_table(kL1, va, kL2, kTXn | kTApt0); /* no EL0, no EL0-exec for the subtree */
  l2_table(va, kL3);
  l3_page(va, kRam + UINT64_C(0x9000), kAp1); /* a user-RW page */
  uint64_t pa = 0U;
  ASSERT_EQ(xlat(va, false, false, &pa, nullptr), OEMU_OK); /* EL1 data unaffected */
  oemu_mmu_fault f{};
  ASSERT_EQ(xlat(va, false, true, &pa, &f), OEMU_OK); /* XN table bit spares EL1 */
  el0();
  ASSERT_EQ(xlat(va, false, false, &pa, &f), OEMU_ERR_FAULT);
  EXPECT_EQ(f.esr, EcBase(OEMU_EXC_EC_DABORT_LOWER) | kIlBit | Perm(3)); /* APT0 */
  el1();
  l1_table(kL1, va, kL2, kTPxn | kTApt1); /* EL1 read-only, no EL1 exec for the subtree */
  ASSERT_EQ(xlat(va, true, false, &pa, &f), OEMU_ERR_FAULT);
  EXPECT_EQ(f.esr, EcBase(OEMU_EXC_EC_DABORT_SAME) | kIlBit | kWnr | Perm(3)); /* APT1 */
  ASSERT_EQ(xlat(va, false, true, &pa, &f), OEMU_ERR_FAULT);
  EXPECT_EQ(f.esr, EcBase(OEMU_EXC_EC_IABORT_SAME) | kIlBit | Perm(3)); /* table PXN */
}

/* --- banding and tagging ---------------------------------------------------------- */

TEST_F(MmuTest, UpperRegionWalksTtbr1) {
  const uint64_t base = ~((UINT64_C(1) << 39U) - 1U);         /* the upper region's first VA */
  l1_block(kL1b, base + 0x1234ULL, UINT64_C(0x80000000), 0U); /* 1 GiB-aligned */
  uint64_t pa = 0U;
  ASSERT_EQ(xlat(base + 0x1234ULL, false, false, &pa, nullptr), OEMU_OK);
  EXPECT_EQ(pa, UINT64_C(0x80001234));
}

TEST_F(MmuTest, TopByteIgnoreHidesTheTag) {
  sr_.tcr_el1 |= kTcrTbi0;
  l1_block(kL1, kRam, kRam, 0U);
  const uint64_t tagged = (UINT64_C(0xFF) << 56U) | (kRam + 0x40ULL);
  uint64_t pa = 0U;
  ASSERT_EQ(xlat(tagged, false, false, &pa, nullptr), OEMU_OK);
  EXPECT_EQ(pa, kRam + 0x40ULL);
  /* With TBI off the tag is part of the address, and a region miss reports
   * exactly the address the guest asked for. */
  sr_.tcr_el1 = 25U | (UINT64_C(25) << 16) | kTcrPs36;
  oemu_mmu_fault f{};
  ASSERT_EQ(xlat(tagged, false, false, &pa, &f), OEMU_ERR_FAULT);
  EXPECT_EQ(f.esr, EcBase(OEMU_EXC_EC_DABORT_SAME) | kIlBit | Trans(1));
  EXPECT_EQ(f.far, tagged);
  /* With TBI on, a tagged address in the gap faults -- and the FAR carries
   * the address as translation sees it, without the tag. */
  sr_.tcr_el1 |= kTcrTbi0;
  const uint64_t tagged_gap = (UINT64_C(0xA5) << 56U) | (UINT64_C(1) << 40U);
  ASSERT_EQ(xlat(tagged_gap, false, false, &pa, &f), OEMU_ERR_FAULT);
  EXPECT_EQ(f.far, UINT64_C(1) << 40U);
}

/* --- the memops wrappers ------------------------------------------------------------ */

TEST_F(MmuTest, ReadForwardsThroughTheWalk) {
  l1_block(kL1, kRam, kRam, 0U);
  write_u64(kRam + 0x80U, 0xC0FFEEULL);
  uint64_t v = 0U;
  ASSERT_EQ(view_.read(view_.ctx, kRam + 0x80U, OEMU_MEM_DWORD, false, &v), OEMU_OK);
  EXPECT_EQ(v, 0xC0FFEEULL);
}

TEST_F(MmuTest, FetchChecksExecutePermission) {
  l1_block(kL1, kRam, kRam, kPxn);
  uint32_t word = 0U;
  ASSERT_EQ(view_.fetch32(view_.ctx, kRam, &word), OEMU_ERR_FAULT);
  EXPECT_EQ(pending_esr(), EcBase(OEMU_EXC_EC_IABORT_SAME) | kIlBit | Perm(1));
  oemu_mmu_fault got{};
  EXPECT_FALSE(oemu_mmu_take_fault(&mmu_, &got)); /* consumed exactly once */
}

TEST_F(MmuTest, AlignmentPolicyFollowsSctlrA) {
  l1_block(kL1, kRam, kRam, 0U);
  write_u64(kRam, 0U);
  uint64_t v = 0U;
  sr_.sctlr_el1 = kSctlrM | kSctlrA;
  ASSERT_EQ(view_.read(view_.ctx, kRam + 1U, OEMU_MEM_WORD, false, &v), OEMU_ERR_FAULT);
  EXPECT_EQ(pending_esr(), EcBase(OEMU_EXC_EC_DABORT_SAME) | kIlBit | kAlign);
  sr_.sctlr_el1 = kSctlrM; /* policy off: the split path serves it */
  ASSERT_EQ(view_.read(view_.ctx, kRam + 1U, OEMU_MEM_WORD, false, &v), OEMU_OK);
}

TEST_F(MmuTest, El0AlignmentFollowsSa0) {
  l1_block(kL1, kRam, kRam, kAp1);
  write_u64(kRam, 0U);
  el0();
  uint64_t v = 0U;
  sr_.sctlr_el1 = kSctlrM | kSctlrSa0;
  ASSERT_EQ(view_.read(view_.ctx, kRam + 2U, OEMU_MEM_WORD, false, &v), OEMU_ERR_FAULT);
  EXPECT_EQ(pending_esr(), EcBase(OEMU_EXC_EC_DABORT_LOWER) | kIlBit | kAlign);
  sr_.sctlr_el1 = kSctlrM;
  ASSERT_EQ(view_.read(view_.ctx, kRam + 2U, OEMU_MEM_WORD, false, &v), OEMU_OK);
}

TEST_F(MmuTest, MisalignedAccessesSplitAcrossPages) {
  /* Two virtual pages, two physical pages far apart: the stitch must come
   * from the mappings, not from adjacency. */
  l1_table(kL1, kRam, kL2, 0U);
  l2_table(kRam, kL3);
  l3_page(kRam, kRam, 0U);
  l3_page(kRam + 0x1000ULL, kRam + 0x200000ULL, 0U);
  ASSERT_EQ(view_.write(view_.ctx, kRam + 0xFFCU, OEMU_MEM_DWORD, UINT64_C(0x1122334455667788)),
            OEMU_OK);
  uint64_t v = 0U;
  ASSERT_EQ(view_.read(view_.ctx, kRam + 0xFFCU, OEMU_MEM_DWORD, false, &v), OEMU_OK);
  EXPECT_EQ(v, UINT64_C(0x1122334455667788));
}

TEST_F(MmuTest, SplitWriteIsAllOrNothing) {
  l1_table(kL1, kRam, kL2, 0U);
  l2_table(kRam, kL3);
  l3_page(kRam, kRam, 0U);
  write_desc(kL3 + idx3(kRam + 0x1000ULL) * 8U, 0U); /* the second half maps to nothing */
  write_u64(kRam, 0U);
  ASSERT_EQ(view_.write(view_.ctx, kRam + 0xFFCU, OEMU_MEM_DWORD, UINT64_C(0xFFFF0000FFFF0000)),
            OEMU_ERR_FAULT);
  EXPECT_EQ(read_u64(kRam), 0U); /* nothing committed to the mapped half */
  oemu_mmu_fault got{};
  ASSERT_TRUE(oemu_mmu_take_fault(&mmu_, &got));
  EXPECT_EQ(got.far, kRam + 0x1000ULL); /* the first failing virtual byte */
  EXPECT_EQ(got.esr, EcBase(OEMU_EXC_EC_DABORT_SAME) | kIlBit | kWnr | Trans(3));
}

TEST_F(MmuTest, UnmappedWriteIsADataAbortWithWnr) {
  l1_table(kL1, kRam, kL2, 0U);
  write_desc(kL2 + idx2(kRam) * 8U, 0U);
  ASSERT_EQ(view_.write(view_.ctx, kRam, OEMU_MEM_WORD, 1U), OEMU_ERR_FAULT);
  EXPECT_EQ(pending_esr(), EcBase(OEMU_EXC_EC_DABORT_SAME) | kIlBit | kWnr | Trans(2));
}

TEST_F(MmuTest, SignExtendedReadsComeOutSigned) {
  l1_block(kL1, kRam, kRam, 0U);
  write_u64(kRam, UINT64_C(0xFFFF000000000F81));
  uint64_t v = 0U;
  ASSERT_EQ(view_.read(view_.ctx, kRam, OEMU_MEM_BYTE, true, &v), OEMU_OK);
  EXPECT_EQ(v, UINT64_C(0xFFFFFFFFFFFFFF81));
}

TEST_F(MmuTest, ValidateChecksEveryPageOfTheRange) {
  l1_table(kL1, kRam, kL2, 0U);
  l2_table(kRam, kL3);
  l3_page(kRam, kRam, 0U);
  l3_page(kRam + 0x1000ULL, kRam + 0x200000ULL, 0U);
  /* A 128-byte probe starting mid-page must cover three pages -- and the
   * third (L3 idx 2) is not mapped. */
  ASSERT_EQ(view_.validate(view_.ctx, kRam + 0xF80U, 0x80U, OEMU_PERM_WRITE), OEMU_OK);
  ASSERT_EQ(view_.validate(view_.ctx, kRam + 0x1FF8U, 16U, OEMU_PERM_WRITE), OEMU_ERR_FAULT);
}

}  // namespace
