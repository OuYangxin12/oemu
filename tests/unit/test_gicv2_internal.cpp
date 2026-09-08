/*
 * White-box tests for the GICv2 priority scan (src/dev/gicv2_internal.h).
 *
 * The scan is the pure decision table the whole device is built on. Pinning it
 * directly -- rather than reconstructing every case through the bus -- lets the
 * tricky invariants be isolated: an empty or fully-masked table yields the
 * spurious id, a line can never preempt an active higher-precedence one (the
 * running-priority floor), equal priorities break toward the lower interrupt id,
 * and only bit0 of the target mask matters for a single-CPU model.
 */
#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

#include "dev/gicv2_internal.h"

namespace {

constexpr unsigned kLines = 64U;

void fill(uint8_t *a, int v) {
  for (unsigned i = 0U; i < OEMU_GICV2_LINES; ++i) {
    a[i] = (uint8_t)v;
  }
}

class GicScan : public ::testing::Test {
 protected:
  uint8_t enable[OEMU_GICV2_LINES]{};
  uint8_t pending[OEMU_GICV2_LINES]{};
  uint8_t active[OEMU_GICV2_LINES]{};
  uint8_t priority[OEMU_GICV2_LINES]{};
  uint8_t target[OEMU_GICV2_LINES]{};
};

TEST_F(GicScan, EmptyTableIsSpurious) {
  fill(enable, 0);
  fill(pending, 0);
  EXPECT_EQ(OEMU_GICV2_SPURIOUS, oemu_gicv2_internal_scan(enable, pending, active, priority,
                                                          target, 256U, 0xFFU, kLines));
}

TEST_F(GicScan, SingleEligibleLineIsChosen) {
  fill(enable, 1);
  fill(pending, 0);
  fill(priority, 0x80);
  fill(target, 1);
  pending[7] = 1;
  EXPECT_EQ(7U, oemu_gicv2_internal_scan(enable, pending, active, priority, target, 256U, 0xFFU,
                                         kLines));
}

TEST_F(GicScan, LowestPriorityValueWins) {
  fill(enable, 1);
  fill(priority, 0xF0);
  fill(target, 1);
  pending[10] = 1; /* 0xF0 */
  priority[10] = 0xF0;
  pending[20] = 1;
  priority[20] = 0x30; /* lower value == higher precedence */
  pending[30] = 1;
  priority[30] = 0x70;
  EXPECT_EQ(20U, oemu_gicv2_internal_scan(enable, pending, active, priority, target, 256U,
                                          0xFFU, kLines));
}

TEST_F(GicScan, TieBreaksToLowerInterruptId) {
  fill(enable, 1);
  fill(priority, 0x40);
  fill(target, 1);
  pending[5] = 1;
  pending[40] = 1; /* same priority, higher id */
  EXPECT_EQ(5U, oemu_gicv2_internal_scan(enable, pending, active, priority, target, 256U, 0xFFU,
                                         kLines));
}

TEST_F(GicScan, PriorityMaskDropsEqualOrHigherValue) {
  fill(enable, 1);
  fill(priority, 0x80);
  fill(target, 1);
  pending[3] = 1;
  // pmr == 0x80 drops a line whose value is >= pmr (a line clears the mask only
  // when its value is strictly below pmr).
  EXPECT_EQ(OEMU_GICV2_SPURIOUS, oemu_gicv2_internal_scan(enable, pending, active, priority,
                                                          target, 256U, 0x80U, kLines));
  priority[3] = 0x7F;
  EXPECT_EQ(3U, oemu_gicv2_internal_scan(enable, pending, active, priority, target, 256U, 0x80U,
                                         kLines));
}

TEST_F(GicScan, RunningPriorityFloorBlocksPreemption) {
  fill(enable, 1);
  fill(priority, 0xF0);
  fill(target, 1);
  pending[9] = 1;
  priority[9] = 0x60;
  // A line at 0x60 cannot beat a running priority of 0x40 (higher precedence).
  EXPECT_EQ(OEMU_GICV2_SPURIOUS, oemu_gicv2_internal_scan(enable, pending, active, priority,
                                                          target, 0x40U, 0xFFU, kLines));
  // ... but it can beat a lower-precedence running priority of 0x80.
  EXPECT_EQ(9U, oemu_gicv2_internal_scan(enable, pending, active, priority, target, 0x80U,
                                         0xFFU, kLines));
}

TEST_F(GicScan, DisabledOrUnpendingOrUnactiveAreSkipped) {
  fill(enable, 1);
  fill(pending, 1);
  fill(priority, 0x40);
  fill(target, 1);
  // Everything qualifies to start.
  EXPECT_EQ(0U, oemu_gicv2_internal_scan(enable, pending, active, priority, target, 256U, 0xFFU,
                                         kLines));
  enable[0] = 0;
  pending[0] = 0;
  // Now line 1 is the lowest id still eligible.
  EXPECT_EQ(1U, oemu_gicv2_internal_scan(enable, pending, active, priority, target, 256U, 0xFFU,
                                         kLines));
}

TEST_F(GicScan, AlreadyActiveLineIsNotReAcknowledged) {
  fill(enable, 1);
  fill(pending, 0);
  fill(priority, 0x40);
  fill(target, 1);
  pending[2] = 1;
  active[2] = 1; /* a line cannot be acknowledged while active */
  EXPECT_EQ(OEMU_GICV2_SPURIOUS, oemu_gicv2_internal_scan(enable, pending, active, priority,
                                                          target, 256U, 0xFFU, kLines));
}

TEST_F(GicScan, OnlyBitZeroOfTargetMatters) {
  fill(enable, 1);
  fill(pending, 0);
  fill(priority, 0x40);
  fill(target, 0);
  pending[4] = 1;
  target[4] = 0x01U; /* bit0 set: targeted at cpu0 */
  EXPECT_EQ(4U, oemu_gicv2_internal_scan(enable, pending, active, priority, target, 256U, 0xFFU,
                                         kLines));
  target[4] = 0x00U;
  EXPECT_EQ(OEMU_GICV2_SPURIOUS, oemu_gicv2_internal_scan(enable, pending, active, priority,
                                                          target, 256U, 0xFFU, kLines));
  target[4] = 0x08U; /* another cpu's bit: still not this core */
  EXPECT_EQ(OEMU_GICV2_SPURIOUS, oemu_gicv2_internal_scan(enable, pending, active, priority,
                                                          target, 256U, 0xFFU, kLines));
}

TEST_F(GicScan, ScanRespectsTheLineCountLimit) {
  fill(enable, 1);
  fill(pending, 0);
  fill(priority, 0x40);
  fill(target, 1);
  pending[40] = 1; /* beyond a 32-line view */
  EXPECT_EQ(OEMU_GICV2_SPURIOUS, oemu_gicv2_internal_scan(enable, pending, active, priority,
                                                          target, 256U, 0xFFU, 32U));
  pending[16] = 1;
  EXPECT_EQ(16U, oemu_gicv2_internal_scan(enable, pending, active, priority, target, 256U,
                                          0xFFU, 32U));
}

}  // namespace
