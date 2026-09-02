// Tests for the allocator seam itself: install/restore semantics and the
// behaviour of the test doubles other suites depend on.
#include "oemu/allocator.h"

#include <gtest/gtest.h>

#include "support/tracking_allocator.h"

namespace {

TEST(Allocator, DefaultIsInstalledInitially) {
  EXPECT_EQ(oemu_allocator_default(), oemu_allocator_get());
}

TEST(Allocator, DefaultProvidesAllThreeFunctions) {
  const oemu_allocator *alloc = oemu_allocator_default();
  ASSERT_NE(nullptr, alloc);
  EXPECT_NE(nullptr, alloc->alloc);
  EXPECT_NE(nullptr, alloc->realloc);
  EXPECT_NE(nullptr, alloc->free);
}

// Exercises the real malloc/realloc/free implementations. Without this the
// default allocator's own code paths are never executed, because every other
// test replaces it with a double.
TEST(Allocator, DefaultImplementationAllocatesGrowsAndFrees) {
  const oemu_allocator *alloc = oemu_allocator_default();

  auto *p = static_cast<unsigned char *>(alloc->alloc(8, alloc->user_data));
  ASSERT_NE(nullptr, p);
  for (int i = 0; i < 8; ++i) {
    p[i] = static_cast<unsigned char>(i);
  }

  auto *grown = static_cast<unsigned char *>(alloc->realloc(p, 64, alloc->user_data));
  ASSERT_NE(nullptr, grown);
  // realloc must preserve the existing contents.
  for (int i = 0; i < 8; ++i) {
    EXPECT_EQ(static_cast<unsigned char>(i), grown[i]);
  }

  alloc->free(grown, alloc->user_data);

  // free(NULL) must be a no-op, matching the C standard.
  alloc->free(nullptr, alloc->user_data);
}

TEST(Allocator, DefaultReallocFromNullActsAsAlloc) {
  const oemu_allocator *alloc = oemu_allocator_default();
  void *p = alloc->realloc(nullptr, 16, alloc->user_data);
  ASSERT_NE(nullptr, p);
  alloc->free(p, alloc->user_data);
}

TEST(Allocator, SetReturnsPreviousAllocator) {
  const oemu_allocator *original = oemu_allocator_get();
  oemu_allocator custom = *oemu_allocator_default();

  const oemu_allocator *previous = oemu_allocator_set(&custom);
  EXPECT_EQ(original, previous);
  EXPECT_EQ(&custom, oemu_allocator_get());

  // Restore, and confirm the restore also reports what it displaced.
  EXPECT_EQ(&custom, oemu_allocator_set(previous));
  EXPECT_EQ(original, oemu_allocator_get());
}

TEST(Allocator, SetNullRestoresDefault) {
  oemu_allocator custom = *oemu_allocator_default();
  oemu_allocator_set(&custom);
  ASSERT_EQ(&custom, oemu_allocator_get());

  oemu_allocator_set(nullptr);
  EXPECT_EQ(oemu_allocator_default(), oemu_allocator_get());
}

TEST(TrackingAllocator, InstallsAndRestoresAroundScope) {
  const oemu_allocator *before = oemu_allocator_get();
  {
    oemu_test::TrackingAllocator tracker;
    EXPECT_NE(before, oemu_allocator_get());
  }
  EXPECT_EQ(before, oemu_allocator_get());
}

TEST(TrackingAllocator, CountsAllocationsAndFrees) {
  oemu_test::TrackingAllocator tracker;
  const oemu_allocator *alloc = oemu_allocator_get();

  void *p = alloc->alloc(32, alloc->user_data);
  ASSERT_NE(nullptr, p);
  EXPECT_EQ(1u, tracker.alloc_count());
  EXPECT_EQ(1u, tracker.live_blocks());
  EXPECT_EQ(32u, tracker.bytes_requested());

  alloc->free(p, alloc->user_data);
  EXPECT_EQ(1u, tracker.free_count());
  EXPECT_EQ(0u, tracker.live_blocks());
  EXPECT_FALSE(tracker.has_leaks());
}

TEST(TrackingAllocator, DetectsALeak) {
  oemu_test::TrackingAllocator tracker;
  const oemu_allocator *alloc = oemu_allocator_get();

  void *p = alloc->alloc(8, alloc->user_data);
  ASSERT_NE(nullptr, p);
  EXPECT_TRUE(tracker.has_leaks());

  alloc->free(p, alloc->user_data);  // clean up for real
  EXPECT_FALSE(tracker.has_leaks());
}

TEST(FailingAllocator, FailsOnlyTheSelectedCall) {
  oemu_test::FailingAllocator failing(2);  // second attempt fails
  const oemu_allocator *alloc = oemu_allocator_get();

  void *first = alloc->alloc(16, alloc->user_data);
  EXPECT_NE(nullptr, first);
  EXPECT_FALSE(failing.did_fail());

  void *second = alloc->alloc(16, alloc->user_data);
  EXPECT_EQ(nullptr, second);
  EXPECT_TRUE(failing.did_fail());

  void *third = alloc->alloc(16, alloc->user_data);
  EXPECT_NE(nullptr, third);

  alloc->free(first, alloc->user_data);
  alloc->free(third, alloc->user_data);
  EXPECT_EQ(3u, failing.call_count());
}

TEST(FailingAllocator, NeverFailsWhenDisabled) {
  oemu_test::FailingAllocator failing(oemu_test::FailingAllocator::kNever);
  const oemu_allocator *alloc = oemu_allocator_get();

  for (int i = 0; i < 5; ++i) {
    void *p = alloc->alloc(8, alloc->user_data);
    ASSERT_NE(nullptr, p);
    alloc->free(p, alloc->user_data);
  }
  EXPECT_FALSE(failing.did_fail());
}

}  // namespace
