/*
 * Test-only allocator utilities.
 *
 * Pure C code cannot be intercepted by gmock (gmock needs virtual functions),
 * so the library exposes an allocator seam instead -- see oemu/allocator.h.
 * These helpers plug into that seam to:
 *
 *   - count allocations and detect leaks (TrackingAllocator);
 *   - make the Nth allocation fail, to cover out-of-memory paths (FailingAllocator).
 */
#ifndef OEMU_TESTS_TRACKING_ALLOCATOR_H
#define OEMU_TESTS_TRACKING_ALLOCATOR_H

#include "oemu/allocator.h"

#include <cstddef>

namespace oemu_test {

/*
 * Installs itself as the process allocator for its lifetime and restores the
 * previous one on destruction (RAII), so a failing test cannot leak the
 * override into the next one.
 */
class TrackingAllocator {
 public:
  TrackingAllocator();
  ~TrackingAllocator();

  TrackingAllocator(const TrackingAllocator &) = delete;
  TrackingAllocator &operator=(const TrackingAllocator &) = delete;

  // Number of successful alloc/realloc-as-alloc calls.
  std::size_t alloc_count() const { return alloc_count_; }
  // Number of free calls with a non-null pointer.
  std::size_t free_count() const { return free_count_; }
  // Number of realloc calls.
  std::size_t realloc_count() const { return realloc_count_; }
  // Blocks currently outstanding; non-zero at teardown means a leak.
  std::size_t live_blocks() const { return live_blocks_; }
  // Total bytes ever requested.
  std::size_t bytes_requested() const { return bytes_requested_; }

  bool has_leaks() const { return live_blocks_ != 0; }

 private:
  static void *Alloc(std::size_t size, void *user_data);
  static void *Realloc(void *ptr, std::size_t new_size, void *user_data);
  static void Free(void *ptr, void *user_data);

  oemu_allocator allocator_{};
  const oemu_allocator *previous_ = nullptr;

  std::size_t alloc_count_ = 0;
  std::size_t realloc_count_ = 0;
  std::size_t free_count_ = 0;
  std::size_t live_blocks_ = 0;
  std::size_t bytes_requested_ = 0;
};

/*
 * Fails the allocation whose 1-based index equals `fail_on_call`; every other
 * allocation succeeds. Use fail_on_call == 1 to fail the very first one, or
 * kNever to disable failures.
 *
 * Freeing always works, so a test can still clean up after an injected failure.
 */
class FailingAllocator {
 public:
  static constexpr std::size_t kNever = 0;

  explicit FailingAllocator(std::size_t fail_on_call);
  ~FailingAllocator();

  FailingAllocator(const FailingAllocator &) = delete;
  FailingAllocator &operator=(const FailingAllocator &) = delete;

  // Allocation attempts seen so far, including the failed one.
  std::size_t call_count() const { return call_count_; }
  bool did_fail() const { return did_fail_; }

  // Changes the failure point and resets the counter.
  void set_fail_on_call(std::size_t fail_on_call);

 private:
  static void *Alloc(std::size_t size, void *user_data);
  static void *Realloc(void *ptr, std::size_t new_size, void *user_data);
  static void Free(void *ptr, void *user_data);

  bool ShouldFail();

  oemu_allocator allocator_{};
  const oemu_allocator *previous_ = nullptr;
  std::size_t fail_on_call_ = kNever;
  std::size_t call_count_ = 0;
  bool did_fail_ = false;
};

}  // namespace oemu_test

#endif  // OEMU_TESTS_TRACKING_ALLOCATOR_H
