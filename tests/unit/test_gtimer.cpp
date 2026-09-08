// Tests for the generic-timer expiry predicate (M5). The counter is a parameter,
// so the whole device -- "is the comparator firing" -- is pinned without a
// running guest or a wall clock. This is the behaviour that decides whether an
// idle guest's jiffies advance or it soft-locks waiting for a tick.

#include "oemu/gtimer.h"

#include <gtest/gtest.h>

TEST(Gtimer, DisabledNeverFires) {
  EXPECT_EQ(0, oemu_gtimer_pending(1000ULL, 0x0ULL, 1ULL)); /* ENABLE clear */
  EXPECT_EQ(0, oemu_gtimer_pending(1000ULL, OEMU_GTIMER_CTL_IMASK, 1ULL));
}

TEST(Gtimer, MaskedNeverFires) {
  // Enabled but IMASK set: the output stays low even long past the comparator.
  EXPECT_EQ(
      0, oemu_gtimer_pending(1000000ULL, OEMU_GTIMER_CTL_ENABLE | OEMU_GTIMER_CTL_IMASK, 1ULL));
}

TEST(Gtimer, FiresExactlyAtAndPastTheComparator) {
  const uint64_t cval = 5000ULL;
  EXPECT_EQ(0, oemu_gtimer_pending(cval - 1ULL, OEMU_GTIMER_CTL_ENABLE, cval)); /* not yet */
  EXPECT_EQ(1, oemu_gtimer_pending(cval, OEMU_GTIMER_CTL_ENABLE, cval));        /* exactly at */
  EXPECT_EQ(1, oemu_gtimer_pending(cval + 1ULL, OEMU_GTIMER_CTL_ENABLE, cval)); /* past */
}

TEST(Gtimer, ZeroComparatorWithCounterZeroFires) {
  // A freshly-armed CVAL of 0 with the counter at 0 is "expired" -- the kernel
  // never leaves a stale comparator like this, but the predicate's rule is
  // simply `counter >= cval` and that is what we assert.
  EXPECT_EQ(1, oemu_gtimer_pending(0ULL, OEMU_GTIMER_CTL_ENABLE, 0ULL));
}

TEST(Gtimer, WrapAroundIsStillExpired) {
  // A 64-bit counter that has wrapped is only a hair past a high comparator:
  // the subtract-with-sign rule says expired, and a plain >= would say so too
  // here. The interesting case is a tiny counter against a huge CVAL it passed
  // via wrap: 0x2 vs 0xFFFFFFFFFFFFFFFF is 3 steps past.
  EXPECT_EQ(1, oemu_gtimer_pending(0x2ULL, OEMU_GTIMER_CTL_ENABLE, 0xFFFFFFFFFFFFFFFFULL));
  EXPECT_EQ(0, oemu_gtimer_pending(0xFFFFFFFFFFFFFFFEULL, OEMU_GTIMER_CTL_ENABLE,
                                   0xFFFFFFFFFFFFFFFFULL)); /* one short, not yet */
}
