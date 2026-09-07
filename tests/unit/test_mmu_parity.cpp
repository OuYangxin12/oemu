/*
 * M3b parity: the TLB may only be faster, never different.
 *
 * The oracle is the bypass itself -- oemu_mmu_internal_walk runs the exact
 * walk the cache fronts, with no lookup, no fill and no counters -- so
 * every access is compared against its own ground truth instead of a
 * hand-typed answer. The sweep is a deterministic LCG (no rand(): a failure
 * must reproduce byte-identically on any machine) over a pseudo-randomly
 * assembled page table: pages, a block, holes, AF=0 leaves, every AP and
 * XN shape, tagged and untagged addresses, both ELs.
 *
 * Faults must match too -- not only the pass/fail bit but the composed ESR
 * and FAR: the record a guest handler reads is part of the behaviour a
 * cache is not allowed to change.
 */
#include "oemu/aspace.h"
#include "oemu/exc.h"
#include "oemu/exec.h"
#include "oemu/mmu.h"
#include "oemu/regs.h"
#include "oemu/sysreg.h"

#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

#include "mmu/mmu_internal.h"

namespace {

constexpr uint64_t kRam = UINT64_C(0x40000000);
constexpr uint64_t kRamSize = UINT64_C(0x1000000);
constexpr uint64_t kUpperPa = UINT64_C(0x80000000);

/* Table plane. T0SZ/T1SZ = 25 -> start level 1; RAM's bit 30 puts every
 * kRam-based low-region VA at L1 index 1. */
constexpr uint64_t kL1 = kRam + 0x100000U;  /* low start table */
constexpr uint64_t kL1b = kRam + 0x101000U; /* high start table */
constexpr uint64_t kL2 = kRam + 0x102000U;  /* low L2 */
constexpr uint64_t kL3 = kRam + 0x103000U;  /* low L3 */

constexpr uint64_t kTable = UINT64_C(3);
constexpr uint64_t kBlock = UINT64_C(1);
constexpr uint64_t kAf = UINT64_C(1) << 10;
constexpr uint64_t kNg = UINT64_C(1) << 11;
constexpr uint64_t kAp1 = UINT64_C(1) << 6;
constexpr uint64_t kAp2 = UINT64_C(1) << 7;
constexpr uint64_t kPxn = UINT64_C(1) << 53;
constexpr uint64_t kXn = UINT64_C(1) << 54;

constexpr uint64_t kSctlrM = UINT64_C(1) << 0;
constexpr uint64_t kTcrTbi0 = UINT64_C(1) << 24;
constexpr uint64_t kTcrPs36 = UINT64_C(1) << 27;

/* The upper region's first VA for a 39-bit space (sign-extended high half). */
constexpr uint64_t kUpper = ~((UINT64_C(1) << 39U) - 1U);

/*
 * Deterministic pseudo-randomness: an explicit LCG (Knuth's MMIX
 * constants). Which slot got which leaf kind is part of the pinned test
 * shape and must be identical on every machine and every run -- so no
 * rand() anywhere in this file.
 */
class Lcg {
 public:
  explicit Lcg(uint64_t seed) : state_(seed) {}
  uint64_t next() {
    state_ = state_ * UINT64_C(6364136223846793005) + UINT64_C(1442695040888963407);
    return state_;
  }
  unsigned below(unsigned bound) { return static_cast<unsigned>(next() % bound); }

 private:
  uint64_t state_;
};

class TlbParity : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(oemu_aspace_init(&as_, 4U), OEMU_OK);
    void *ram = nullptr;
    ASSERT_EQ(oemu_aspace_map_ram(&as_, kRam, kRamSize, OEMU_PERM_ALL, &ram), OEMU_OK);
    auto *bytes = static_cast<unsigned char *>(ram);
    for (size_t i = 0U; i < static_cast<size_t>(kRamSize); ++i) {
      bytes[i] = static_cast<unsigned char>(i * 7U + 3U);
    }
    ASSERT_EQ(oemu_aspace_map_ram(&as_, kUpperPa, 0x1000U, OEMU_PERM_ALL, &upper_), OEMU_OK);
    ASSERT_EQ(oemu_cpu_init(&cpu_, kRam, kRam + 0x8000U), OEMU_OK);
    oemu_sysregs_init(&sr_, &cpu_.regs, OEMU_EL1);
    el1();
    const oemu_memops bus = oemu_aspace_memops(&as_);
    oemu_mmu_init(&mmu_, &sr_, &bus);
    /* Geometry: 39-bit both regions, 36-bit PA, tags ignored in the low one. */
    sr_.tcr_el1 = 25U | (UINT64_C(25) << 16) | kTcrPs36 | kTcrTbi0;
    sr_.ttbr0_el1 = kL1;
    sr_.ttbr1_el1 = kL1b;
    sr_.sctlr_el1 = kSctlrM;
  }
  void TearDown() override { oemu_aspace_dispose(&as_); }

  void el0() { sr_.pstate = OEMU_PSTATE_M_EL0T | (0xFULL << 6U); }
  void el1() { sr_.pstate = OEMU_PSTATE_M_EL1H | (0xFULL << 6U); }

  void put(uint64_t pa, uint64_t v) {
    EXPECT_EQ(oemu_aspace_write(&as_, pa, OEMU_MEM_DWORD, v), OEMU_OK);
  }

  static unsigned idx1(uint64_t va) { return static_cast<unsigned>((va >> 30U) & 0x1FFU); }
  static unsigned idx2(uint64_t va) { return static_cast<unsigned>((va >> 21U) & 0x1FFU); }
  static unsigned idx3(uint64_t va) { return static_cast<unsigned>((va >> 12U) & 0x1FFU); }

  /* The low chain: L1[idx1(kRam)] -> kL2, kL2[0] -> kL3. */
  void buildLowChain() {
    put(kL1 + idx1(kRam) * 8U, kTable | kL2);
    put(kL2 + idx2(kRam) * 8U, kTable | kL3);
  }

  OEMU_NODISCARD oemu_status oracle(uint64_t va, oemu_el cur, bool w, bool f, uint64_t *pa,
                                    oemu_mmu_fault *fl) {
    return oemu_mmu_internal_walk(&mmu_, va, cur, w, f, pa, fl);
  }
  OEMU_NODISCARD oemu_status served(uint64_t va, oemu_el cur, bool w, bool f, uint64_t *pa,
                                    oemu_mmu_fault *fl) {
    /* The public entry reads PSTATE for the regime; make it say what the
     * comparison means before calling it. */
    if (cur == OEMU_EL0) {
      el0();
    } else {
      el1();
    }
    return oemu_mmu_translate(&mmu_, va, w, f, pa, fl);
  }

  /* One oracle-vs-cache comparison at one address and access kind. */
  void expectParity(uint64_t va, oemu_el cur, bool w, bool f) {
    oemu_mmu_fault wf{};
    oemu_mmu_fault cf{};
    uint64_t wpa = 0U;
    uint64_t cpa = 0U;
    const oemu_status want = oracle(va, cur, w, f, &wpa, &wf);
    const oemu_status got = served(va, cur, w, f, &cpa, &cf);
    EXPECT_EQ(want, got) << "status @ va=" << va;
    if (want == OEMU_OK) {
      EXPECT_EQ(wpa, cpa) << "pa @ va=" << va;
    }
    if (want == OEMU_ERR_FAULT) {
      EXPECT_EQ(wf.esr, cf.esr) << "esr @ va=" << va;
      EXPECT_EQ(wf.far, cf.far) << "far @ va=" << va;
    }
  }

  oemu_aspace as_{};
  oemu_cpu cpu_{};
  oemu_sysregs sr_{};
  oemu_mmu mmu_{};
  void *upper_ = nullptr;
};

/* --- the sweep --------------------------------------------------------------- */

TEST_F(TlbParity, RandomSweepEveryShapeTwicePerRound) {
  buildLowChain();
  Lcg rng(UINT64_C(0xB50C2F37A17E8D41));

  /* The low L3 gets every leaf shape, in LCG order, plus holes. */
  constexpr unsigned kSlots = 128U;
  for (unsigned i = 0U; i < kSlots; ++i) {
    const uint64_t leaf_pa = kRam + 0x800000ULL + (static_cast<uint64_t>(i) << 12);
    switch (rng.below(7U)) {
      case 0U:
        break; /* hole */
      case 1U:
        put(kL3 + i * 8U, kBlock | leaf_pa); /* AF=0: a fault, not a mapping */
        break;
      case 2U:
        put(kL3 + i * 8U, kBlock | leaf_pa | kAf);
        break;
      case 3U:
        put(kL3 + i * 8U, kBlock | leaf_pa | kAf | kAp1);
        break;
      case 4U:
        put(kL3 + i * 8U, kBlock | leaf_pa | kAf | kAp1 | kAp2);
        break;
      case 5U:
        put(kL3 + i * 8U, kBlock | leaf_pa | kAf | kAp1 | kXn | kNg);
        break;
      default:
        put(kL3 + i * 8U, kBlock | leaf_pa | kAf | kPxn);
        break;
    }
  }
  /* A level-2 block beside the chain: it owns VA bits [29:21] == 1, a
   * two-megabyte leaf two megabytes past kRam. */
  put(kL2 + idx2(kRam + 0x200000ULL) * 8U, kBlock | (kRam + 0xC00000ULL) | kAf | kAp1 | kNg);

  const uint64_t hits0 = mmu_.tlb_hits;
  const uint64_t misses0 = mmu_.tlb_misses;
  for (unsigned round = 0U; round < 3U; ++round) {
    for (unsigned i = 0U; i < kSlots; ++i) {
      const uint64_t word = rng.next();
      const uint64_t base = kRam + (static_cast<uint64_t>(i) << 12);
      const bool tagged = (word & 0x80000000ULL) != 0U;
      const uint64_t va =
          tagged ? (base | (UINT64_C(0xA5) << 56)) : (base + (word & UINT64_C(0xFC)));
      const auto el = ((word >> 8U) & 1U) != 0U ? OEMU_EL0 : OEMU_EL1;
      const bool is_write = (word & 0x20U) != 0U;
      const bool is_fetch = (word & 0x2U) != 0U;
      expectParity(va, el, is_write, is_fetch);
      /* Immediately again: the second pass is the one the cache may serve,
       * so parity must survive the fill, not just the miss. */
      expectParity(va, el, is_write, is_fetch);
    }
    /* Inside a big leaf, offset by offset: a hit must re-add the offset the
     * walk would have, at every offset in the block. */
    for (unsigned j = 0U; j < 32U; ++j) {
      const uint64_t va = kRam + 0x200000ULL + (static_cast<uint64_t>(j) << 12) + 8U;
      const bool w = (j & 1U) != 0U;
      expectParity(va, OEMU_EL1, w, false);
    }
  }
  /* The sweep only means something if the cache actually served it. */
  EXPECT_GT(mmu_.tlb_hits - hits0, UINT64_C(300)) << "a parity sweep that never hit is vacuous";
  EXPECT_GT(mmu_.tlb_misses - misses0, UINT64_C(100));
}

/* --- the upper region, with the honest banding -------------------------------- */

TEST_F(TlbParity, UpperRegionParitysLikeTheLower) {
  /* A 1 GiB block in the high start table, mapped the way MmuTest does it.
   * A single VA pair is enough: the block leaf exercises the region-split
   * and tag-strip paths identically from both cache and walk. */
  const uint64_t va = kUpper + 0x1234ULL;
  put(kL1b + idx1(va) * 8U, kBlock | kUpperPa | kAf | kAp1);
  for (unsigned j = 0U; j < 4U; ++j) {
    expectParity(kUpper + 0x1234ULL + (static_cast<uint64_t>(j) << 12), OEMU_EL1, false, false);
    expectParity(kUpper + 0x1234ULL + (static_cast<uint64_t>(j) << 12), OEMU_EL1, false, false);
  }
  EXPECT_GT(mmu_.tlb_hits, UINT64_C(2));
}

/* --- invalidation ------------------------------------------------------------ */

TEST_F(TlbParity, StaleUntilInvalidatedExactAfter) {
  buildLowChain();
  put(kL3, kBlock | (kRam + 0x900000ULL) | kAf);
  uint64_t pa = 0U;
  oemu_mmu_fault f{};
  ASSERT_EQ(served(kRam + 8U, OEMU_EL1, false, false, &pa, &f), OEMU_OK);
  EXPECT_EQ(pa, kRam + 0x900008ULL);
  /* Break-before-make: the rewrite is exactly as stale as the architecture
   * says -- until the invalidation. A guest that skips the TLBI asked for
   * the old leaf and gets it; that is not a bug to paper over, so the
   * staleness is asserted, not hidden. */
  put(kL3, kBlock | (kRam + 0x901000ULL) | kAf);
  ASSERT_EQ(served(kRam + 8U, OEMU_EL1, false, false, &pa, &f), OEMU_OK);
  EXPECT_EQ(pa, kRam + 0x900008ULL) << "the cache must serve the old leaf until TLBI";
  oemu_mmu_flush_all(&mmu_);
  ASSERT_EQ(served(kRam + 8U, OEMU_EL1, false, false, &pa, &f), OEMU_OK);
  EXPECT_EQ(pa, kRam + 0x901008ULL);
  uint64_t opa = 0U;
  oemu_mmu_fault of{};
  ASSERT_EQ(oracle(kRam + 8U, OEMU_EL1, false, false, &opa, &of), OEMU_OK);
  EXPECT_EQ(pa, opa) << "after the flush the cache and the walk must agree again";
}

/* --- the cache itself ---------------------------------------------------------- */

TEST_F(TlbParity, CollidingEvictionAndTagLayoutAreAsDocumented) {
  /* Two VAs that share a set (bits [23:12]) but differ above it: va0 keys
   * one leaf through L2 slot idx1(kRam)/idx2(kRam)/idx3(kRam), va1 (one
   * L1 index up) keys another through a different L2 table -- yet both
   * land in TLB set 0, so the second must evict the first. */
  put(kL1 + idx1(kRam) * 8U, kTable | kL2);
  put(kL2 + idx2(kRam) * 8U, kTable | kL3);
  put(kL3 + idx3(kRam) * 8U, kBlock | (kRam + 0x900000ULL) | kAf);
  const uint64_t va1 = kRam + 0x1000000ULL; /* one megabyte up: same set index 0 */
  ASSERT_EQ(idx1(va1), idx1(kRam));         /* same L1 slot is the whole trick */
  ASSERT_EQ((va1 >> 12U) & 0xFFFU, (kRam >> 12U) & 0xFFFU);
  const uint64_t kL3b = kL3 + 0x1000U; /* a second L3 table: same in-table
                                        * slot, different object -- the L2
                                        * index moved, the set did not */
  put(kL2 + idx2(va1) * 8U, kTable | kL3b);
  put(kL3b + idx3(va1) * 8U, kBlock | (kRam + 0x910000ULL) | kAf | kAp1 | kNg);

  uint64_t pa = 0U;
  oemu_mmu_fault f{};
  ASSERT_EQ(served(kRam, OEMU_EL1, false, false, &pa, &f), OEMU_OK);
  oemu_tlb_entry e{};
  ASSERT_TRUE(oemu_mmu_internal_tlb_peek(&mmu_, 0U, &e)); /* the entry lives in set 0 */
  EXPECT_EQ(e.va, kRam);
  EXPECT_EQ(e.level, 3U);
  EXPECT_EQ(e.flags & OEMU_TLB_NG, 0U); /* this leaf is global */
  EXPECT_EQ(e.flags & OEMU_TLB_AP, 0U); /* AP=0b00 */
  EXPECT_EQ(e.vmid, 0U);                /* no stage 2 */
  EXPECT_EQ(e.asid, 0U);                /* TTBR[63:48] was zero */

  /* Now the colliding neighbour: it must overwrite set 0's tag. */
  ASSERT_EQ(served(va1 + 8U, OEMU_EL1, false, false, &pa, &f), OEMU_OK);
  EXPECT_EQ(pa, kRam + 0x910008ULL);
  ASSERT_TRUE(oemu_mmu_internal_tlb_peek(&mmu_, 0U, &e));
  EXPECT_EQ(e.va, va1); /* evicted: the survivor is the second visitor */
  EXPECT_NE(e.flags & OEMU_TLB_NG, 0U);
  EXPECT_EQ(e.flags & OEMU_TLB_AP, 0x02U); /* AP=0b01, stored left-shifted one */
  /* And kRam is no longer cached: re-serving it must re-walk, not hit the
   * neighbour's tag. */
  EXPECT_EQ(mmu_.tlb_misses > 0U, true);
  ASSERT_EQ(served(kRam, OEMU_EL1, false, false, &pa, &f), OEMU_OK);
  EXPECT_EQ(pa, kRam + 0x900000ULL);
  /* Peeking out of range answers false, never garbage. */
  EXPECT_FALSE(oemu_mmu_internal_tlb_peek(&mmu_, OEMU_MMU_TLB_ENTRIES, &e));
}

}  // namespace
