// Black-box tests for the public buffer API, including out-of-memory paths
// injected through the allocator seam.
#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "oemu/buffer.h"
#include "oemu/status.h"
#include "support/tracking_allocator.h"

namespace {

// Fixture: installs the tracking allocator, always disposes the buffer, and
// asserts on teardown that the module leaked nothing. Every test below inherits
// that leak check for free.
class BufferTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_EQ(OEMU_OK, oemu_buffer_init(&buf_, 0)); }

  void TearDown() override {
    oemu_buffer_dispose(&buf_);
    EXPECT_EQ(0u, tracker_.live_blocks()) << "buffer module leaked memory";
  }

  std::string Contents() const {
    const unsigned char *data = oemu_buffer_data(&buf_);
    const std::size_t len = oemu_buffer_len(&buf_);
    if (data == nullptr || len == 0) {
      return {};
    }
    return std::string(reinterpret_cast<const char *>(data), len);
  }

  oemu_test::TrackingAllocator tracker_;
  oemu_buffer buf_{};
};

// --- init / dispose ----------------------------------------------------------

TEST_F(BufferTest, InitZeroCapacityDoesNotAllocate) {
  oemu_buffer local{};
  const std::size_t before = tracker_.alloc_count();

  ASSERT_EQ(OEMU_OK, oemu_buffer_init(&local, 0));
  EXPECT_EQ(before, tracker_.alloc_count());
  EXPECT_EQ(nullptr, oemu_buffer_data(&local));
  EXPECT_EQ(0u, oemu_buffer_len(&local));
  EXPECT_EQ(0u, oemu_buffer_capacity(&local));

  oemu_buffer_dispose(&local);
}

TEST_F(BufferTest, InitWithCapacityAllocatesExactly) {
  oemu_buffer local{};
  ASSERT_EQ(OEMU_OK, oemu_buffer_init(&local, 64));
  EXPECT_EQ(64u, oemu_buffer_capacity(&local));
  EXPECT_EQ(0u, oemu_buffer_len(&local));
  EXPECT_NE(nullptr, oemu_buffer_data(&local));
  oemu_buffer_dispose(&local);
}

TEST_F(BufferTest, InitRejectsNullBuffer) {
  EXPECT_EQ(OEMU_ERR_INVALID_ARG, oemu_buffer_init(nullptr, 16));
}

TEST_F(BufferTest, DisposeIsIdempotentAndNullSafe) {
  oemu_buffer local{};
  ASSERT_EQ(OEMU_OK, oemu_buffer_init(&local, 16));
  oemu_buffer_dispose(&local);
  oemu_buffer_dispose(&local);  // second call must be harmless
  oemu_buffer_dispose(nullptr);
  EXPECT_EQ(0u, oemu_buffer_capacity(&local));
  EXPECT_EQ(nullptr, oemu_buffer_data(&local));
}

// --- append ------------------------------------------------------------------

TEST_F(BufferTest, AppendBytesStoresThem) {
  ASSERT_EQ(OEMU_OK, oemu_buffer_append(&buf_, "abc", 3));
  EXPECT_EQ(3u, oemu_buffer_len(&buf_));
  EXPECT_EQ("abc", Contents());
}

TEST_F(BufferTest, AppendConcatenatesInOrder) {
  ASSERT_EQ(OEMU_OK, oemu_buffer_append_str(&buf_, "hello"));
  ASSERT_EQ(OEMU_OK, oemu_buffer_append_str(&buf_, ", "));
  ASSERT_EQ(OEMU_OK, oemu_buffer_append_str(&buf_, "world"));
  EXPECT_EQ("hello, world", Contents());
}

TEST_F(BufferTest, AppendZeroSizeSucceedsAndChangesNothing) {
  ASSERT_EQ(OEMU_OK, oemu_buffer_append_str(&buf_, "x"));
  const std::size_t len = oemu_buffer_len(&buf_);

  EXPECT_EQ(OEMU_OK, oemu_buffer_append(&buf_, "ignored", 0));
  // A zero size is a no-op even with a NULL pointer.
  EXPECT_EQ(OEMU_OK, oemu_buffer_append(&buf_, nullptr, 0));
  EXPECT_EQ(len, oemu_buffer_len(&buf_));
}

TEST_F(BufferTest, AppendRejectsNullArguments) {
  EXPECT_EQ(OEMU_ERR_INVALID_ARG, oemu_buffer_append(nullptr, "a", 1));
  EXPECT_EQ(OEMU_ERR_INVALID_ARG, oemu_buffer_append(&buf_, nullptr, 1));
  EXPECT_EQ(OEMU_ERR_INVALID_ARG, oemu_buffer_append_str(&buf_, nullptr));
  EXPECT_EQ(OEMU_ERR_INVALID_ARG, oemu_buffer_append_str(nullptr, "a"));
}

TEST_F(BufferTest, AppendHandlesEmbeddedNulBytes) {
  const unsigned char raw[] = {'a', 0, 'b', 0, 'c'};
  ASSERT_EQ(OEMU_OK, oemu_buffer_append(&buf_, raw, sizeof(raw)));
  EXPECT_EQ(5u, oemu_buffer_len(&buf_));
  EXPECT_EQ(0, std::memcmp(oemu_buffer_data(&buf_), raw, sizeof(raw)));
}

TEST_F(BufferTest, AppendStrIgnoresTerminator) {
  ASSERT_EQ(OEMU_OK, oemu_buffer_append_str(&buf_, "abcd"));
  EXPECT_EQ(4u, oemu_buffer_len(&buf_));
}

TEST_F(BufferTest, ManyAppendsGrowCapacityGeometrically) {
  for (int i = 0; i < 1000; ++i) {
    ASSERT_EQ(OEMU_OK, oemu_buffer_append(&buf_, "x", 1)) << "at iteration " << i;
  }
  EXPECT_EQ(1000u, oemu_buffer_len(&buf_));
  EXPECT_GE(oemu_buffer_capacity(&buf_), 1000u);
  // Geometric growth must keep reallocation count logarithmic, not linear.
  EXPECT_LT(tracker_.realloc_count(), 40u);
}

// --- appendf -----------------------------------------------------------------

TEST_F(BufferTest, AppendfFormatsValues) {
  ASSERT_EQ(OEMU_OK, oemu_buffer_appendf(&buf_, "%s=%d", "n", 42));
  EXPECT_EQ("n=42", Contents());
}

TEST_F(BufferTest, AppendfAppendsAfterExistingContent) {
  ASSERT_EQ(OEMU_OK, oemu_buffer_append_str(&buf_, "pre:"));
  ASSERT_EQ(OEMU_OK, oemu_buffer_appendf(&buf_, "%03d", 7));
  EXPECT_EQ("pre:007", Contents());
}

TEST_F(BufferTest, AppendfEmptyResultIsNoOp) {
  ASSERT_EQ(OEMU_OK, oemu_buffer_appendf(&buf_, "%s", ""));
  EXPECT_EQ(0u, oemu_buffer_len(&buf_));
}

TEST_F(BufferTest, AppendfDoesNotCountTheTerminator) {
  ASSERT_EQ(OEMU_OK, oemu_buffer_appendf(&buf_, "abc"));
  EXPECT_EQ(3u, oemu_buffer_len(&buf_));
  ASSERT_EQ(OEMU_OK, oemu_buffer_append_str(&buf_, "d"));
  EXPECT_EQ("abcd", Contents()) << "terminator must not end up inside the data";
}

TEST_F(BufferTest, AppendfHandlesLongOutput) {
  const std::string long_arg(5000, 'z');
  ASSERT_EQ(OEMU_OK, oemu_buffer_appendf(&buf_, "%s", long_arg.c_str()));
  EXPECT_EQ(5000u, oemu_buffer_len(&buf_));
  EXPECT_EQ(long_arg, Contents());
}

TEST_F(BufferTest, AppendfRejectsNullArguments) {
  EXPECT_EQ(OEMU_ERR_INVALID_ARG, oemu_buffer_appendf(nullptr, "x"));
  EXPECT_EQ(OEMU_ERR_INVALID_ARG, oemu_buffer_appendf(&buf_, nullptr));
}

// --- cstr --------------------------------------------------------------------

TEST_F(BufferTest, CstrTerminatesContents) {
  ASSERT_EQ(OEMU_OK, oemu_buffer_append_str(&buf_, "text"));
  const char *s = oemu_buffer_cstr(&buf_);
  ASSERT_NE(nullptr, s);
  EXPECT_STREQ("text", s);
  EXPECT_EQ(4u, oemu_buffer_len(&buf_)) << "terminator must not change len";
}

TEST_F(BufferTest, CstrOnEmptyBufferReturnsEmptyString) {
  const char *s = oemu_buffer_cstr(&buf_);
  ASSERT_NE(nullptr, s);
  EXPECT_STREQ("", s);
}

TEST_F(BufferTest, CstrRejectsNullBuffer) { EXPECT_EQ(nullptr, oemu_buffer_cstr(nullptr)); }

TEST_F(BufferTest, AppendAfterCstrOverwritesTerminator) {
  ASSERT_EQ(OEMU_OK, oemu_buffer_append_str(&buf_, "ab"));
  ASSERT_STREQ("ab", oemu_buffer_cstr(&buf_));
  ASSERT_EQ(OEMU_OK, oemu_buffer_append_str(&buf_, "cd"));
  EXPECT_STREQ("abcd", oemu_buffer_cstr(&buf_));
}

// --- reserve / clear ---------------------------------------------------------

TEST_F(BufferTest, ReserveRaisesCapacityWithoutChangingLength) {
  ASSERT_EQ(OEMU_OK, oemu_buffer_reserve(&buf_, 256));
  EXPECT_GE(oemu_buffer_capacity(&buf_), 256u);
  EXPECT_EQ(0u, oemu_buffer_len(&buf_));
}

TEST_F(BufferTest, ReserveDoesNotShrink) {
  ASSERT_EQ(OEMU_OK, oemu_buffer_reserve(&buf_, 512));
  const std::size_t cap = oemu_buffer_capacity(&buf_);
  ASSERT_EQ(OEMU_OK, oemu_buffer_reserve(&buf_, 1));
  EXPECT_EQ(cap, oemu_buffer_capacity(&buf_));
}

TEST_F(BufferTest, ReserveRejectsNullBuffer) {
  EXPECT_EQ(OEMU_ERR_INVALID_ARG, oemu_buffer_reserve(nullptr, 8));
}

TEST_F(BufferTest, ReserveOverflowIsReported) {
  ASSERT_EQ(OEMU_OK, oemu_buffer_append_str(&buf_, "x"));
  EXPECT_EQ(OEMU_ERR_OVERFLOW, oemu_buffer_reserve(&buf_, SIZE_MAX));
}

TEST_F(BufferTest, ClearResetsLengthButKeepsCapacity) {
  ASSERT_EQ(OEMU_OK, oemu_buffer_append_str(&buf_, "some data"));
  const std::size_t cap = oemu_buffer_capacity(&buf_);

  oemu_buffer_clear(&buf_);
  EXPECT_EQ(0u, oemu_buffer_len(&buf_));
  EXPECT_EQ(cap, oemu_buffer_capacity(&buf_));

  // Reusing a cleared buffer must not reallocate.
  const std::size_t reallocs = tracker_.realloc_count();
  ASSERT_EQ(OEMU_OK, oemu_buffer_append_str(&buf_, "new"));
  EXPECT_EQ("new", Contents());
  EXPECT_EQ(reallocs, tracker_.realloc_count());
}

TEST_F(BufferTest, ClearIsNullSafe) { oemu_buffer_clear(nullptr); }

// --- accessors on NULL -------------------------------------------------------

TEST(BufferAccessors, TolerateNullBuffer) {
  EXPECT_EQ(0u, oemu_buffer_len(nullptr));
  EXPECT_EQ(0u, oemu_buffer_capacity(nullptr));
  EXPECT_EQ(nullptr, oemu_buffer_data(nullptr));
}

// --- out-of-memory paths -----------------------------------------------------
//
// These use FailingAllocator rather than the tracking fixture, so they are
// plain TESTs. This is where the allocator seam pays for itself: the OOM
// branches are unreachable otherwise.

TEST(BufferOom, InitReportsAllocationFailure) {
  oemu_test::FailingAllocator failing(1);
  oemu_buffer local{};
  EXPECT_EQ(OEMU_ERR_NO_MEMORY, oemu_buffer_init(&local, 32));
  EXPECT_EQ(nullptr, oemu_buffer_data(&local)) << "failed init must leave no dangling pointer";
  EXPECT_EQ(0u, oemu_buffer_capacity(&local));
  oemu_buffer_dispose(&local);
}

TEST(BufferOom, AppendReportsGrowthFailure) {
  oemu_test::FailingAllocator failing(oemu_test::FailingAllocator::kNever);
  oemu_buffer local{};
  ASSERT_EQ(OEMU_OK, oemu_buffer_init(&local, 0));

  failing.set_fail_on_call(1);  // the realloc inside the first append fails
  EXPECT_EQ(OEMU_ERR_NO_MEMORY, oemu_buffer_append_str(&local, "data"));
  EXPECT_EQ(0u, oemu_buffer_len(&local)) << "failed append must not change length";

  oemu_buffer_dispose(&local);
}

TEST(BufferOom, BufferStaysUsableAfterAFailedAppend) {
  oemu_test::FailingAllocator failing(oemu_test::FailingAllocator::kNever);
  oemu_buffer local{};
  ASSERT_EQ(OEMU_OK, oemu_buffer_init(&local, 4));
  ASSERT_EQ(OEMU_OK, oemu_buffer_append_str(&local, "ab"));

  // Force the growth needed by a long append to fail.
  failing.set_fail_on_call(1);
  ASSERT_EQ(OEMU_ERR_NO_MEMORY, oemu_buffer_append_str(&local, "cccccccccccccccccccc"));

  // Earlier content must be intact and further work must succeed.
  failing.set_fail_on_call(oemu_test::FailingAllocator::kNever);
  ASSERT_EQ(OEMU_OK, oemu_buffer_append_str(&local, "d"));
  EXPECT_STREQ("abd", oemu_buffer_cstr(&local));

  oemu_buffer_dispose(&local);
}

TEST(BufferOom, CstrReturnsNullWhenTerminatorCannotBeAllocated) {
  oemu_test::FailingAllocator failing(oemu_test::FailingAllocator::kNever);
  oemu_buffer local{};
  ASSERT_EQ(OEMU_OK, oemu_buffer_init(&local, 0));

  failing.set_fail_on_call(1);
  EXPECT_EQ(nullptr, oemu_buffer_cstr(&local));

  oemu_buffer_dispose(&local);
}

TEST(BufferOom, AppendfReportsGrowthFailure) {
  oemu_test::FailingAllocator failing(oemu_test::FailingAllocator::kNever);
  oemu_buffer local{};
  ASSERT_EQ(OEMU_OK, oemu_buffer_init(&local, 0));

  failing.set_fail_on_call(1);
  EXPECT_EQ(OEMU_ERR_NO_MEMORY, oemu_buffer_appendf(&local, "%d", 12345));

  oemu_buffer_dispose(&local);
}

}  // namespace
