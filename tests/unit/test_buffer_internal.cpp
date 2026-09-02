// White-box tests: exercise the buffer module's internal growth policy through
// src/buffer/buffer_internal.h.
//
// This is the pattern that keeps internal logic testable in a pure C project.
// Instead of marking these helpers `static` (unreachable from a test) or having
// the test #include "buffer.c" (fragile), the module publishes its internals in
// a private header that only tests and the module itself consume.
#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

#include "buffer/buffer_internal.h"
#include "oemu/status.h"

namespace {

constexpr std::size_t kSizeMax = std::numeric_limits<std::size_t>::max();

// --- checked_add -------------------------------------------------------------

TEST(BufferInternalCheckedAdd, AddsNormalValues) {
  std::size_t out = 0;
  ASSERT_EQ(OEMU_OK, oemu_buffer_internal_checked_add(2, 3, &out));
  EXPECT_EQ(5u, out);
}

TEST(BufferInternalCheckedAdd, HandlesZero) {
  std::size_t out = 123;
  ASSERT_EQ(OEMU_OK, oemu_buffer_internal_checked_add(0, 0, &out));
  EXPECT_EQ(0u, out);
}

TEST(BufferInternalCheckedAdd, AcceptsTheExactBoundary) {
  std::size_t out = 0;
  ASSERT_EQ(OEMU_OK, oemu_buffer_internal_checked_add(kSizeMax - 1, 1, &out));
  EXPECT_EQ(kSizeMax, out);
}

TEST(BufferInternalCheckedAdd, DetectsOverflowByOne) {
  std::size_t out = 0;
  EXPECT_EQ(OEMU_ERR_OVERFLOW, oemu_buffer_internal_checked_add(kSizeMax, 1, &out));
}

TEST(BufferInternalCheckedAdd, DetectsOverflowOfTwoLargeValues) {
  std::size_t out = 0;
  EXPECT_EQ(OEMU_ERR_OVERFLOW,
            oemu_buffer_internal_checked_add(kSizeMax / 2 + 2, kSizeMax / 2 + 2, &out));
}

TEST(BufferInternalCheckedAdd, RejectsNullOutput) {
  EXPECT_EQ(OEMU_ERR_INVALID_ARG, oemu_buffer_internal_checked_add(1, 1, nullptr));
}

// --- grow_capacity -----------------------------------------------------------

TEST(BufferInternalGrowCapacity, KeepsCapacityWhenAlreadySufficient) {
  std::size_t out = 0;
  ASSERT_EQ(OEMU_OK, oemu_buffer_internal_grow_capacity(100, 50, &out));
  EXPECT_EQ(100u, out);
}

TEST(BufferInternalGrowCapacity, KeepsCapacityOnExactFit) {
  std::size_t out = 0;
  ASSERT_EQ(OEMU_OK, oemu_buffer_internal_grow_capacity(64, 64, &out));
  EXPECT_EQ(64u, out) << "an exact fit must not trigger growth";
}

TEST(BufferInternalGrowCapacity, FirstGrowthUsesTheMinimum) {
  std::size_t out = 0;
  ASSERT_EQ(OEMU_OK, oemu_buffer_internal_grow_capacity(0, 1, &out));
  EXPECT_EQ(OEMU_BUFFER_MIN_CAP, out);
}

TEST(BufferInternalGrowCapacity, GrowsGeometricallyByOneAndAHalf) {
  std::size_t out = 0;
  // 64 -> 96 is enough for 80, so no further doubling should happen.
  ASSERT_EQ(OEMU_OK, oemu_buffer_internal_grow_capacity(64, 80, &out));
  EXPECT_EQ(96u, out);
}

TEST(BufferInternalGrowCapacity, GrowsRepeatedlyUntilRequirementIsMet) {
  std::size_t out = 0;
  ASSERT_EQ(OEMU_OK, oemu_buffer_internal_grow_capacity(16, 1000, &out));
  EXPECT_GE(out, 1000u);
  // Geometric growth should not overshoot wildly.
  EXPECT_LT(out, 2000u);
}

TEST(BufferInternalGrowCapacity, SatisfiesHugeRequirementNearTheLimit) {
  std::size_t out = 0;
  // Geometric growth cannot reach this, so the policy falls back to the exact
  // size instead of overflowing.
  ASSERT_EQ(OEMU_OK, oemu_buffer_internal_grow_capacity(16, kSizeMax, &out));
  EXPECT_EQ(kSizeMax, out);
}

TEST(BufferInternalGrowCapacity, RejectsNullOutput) {
  EXPECT_EQ(OEMU_ERR_INVALID_ARG, oemu_buffer_internal_grow_capacity(0, 1, nullptr));
}

// Type/value-parameterised sweep: the returned capacity must always cover the
// requirement and never regress below the current capacity.
class GrowCapacityInvariant
    : public ::testing::TestWithParam<std::pair<std::size_t, std::size_t>> {};

TEST_P(GrowCapacityInvariant, ResultCoversRequirementAndNeverShrinks) {
  const auto [current, required] = GetParam();
  std::size_t out = 0;
  ASSERT_EQ(OEMU_OK, oemu_buffer_internal_grow_capacity(current, required, &out));
  EXPECT_GE(out, required) << "capacity must cover the requirement";
  EXPECT_GE(out, current) << "capacity must never shrink";
}

INSTANTIATE_TEST_SUITE_P(
    Sweep, GrowCapacityInvariant,
    ::testing::Values(std::make_pair(0u, 0u), std::make_pair(0u, 1u),
                      std::make_pair(0u, 15u), std::make_pair(0u, 16u),
                      std::make_pair(0u, 17u), std::make_pair(16u, 17u),
                      std::make_pair(16u, 4096u), std::make_pair(1024u, 1025u),
                      std::make_pair(kSizeMax, 1u), std::make_pair(kSizeMax / 2, kSizeMax / 2 + 1)));

}  // namespace
