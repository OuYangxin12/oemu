#include "support/tracking_allocator.h"

#include <cstdlib>

namespace oemu_test {

// --- TrackingAllocator -------------------------------------------------------

TrackingAllocator::TrackingAllocator() {
  allocator_.alloc = &TrackingAllocator::Alloc;
  allocator_.realloc = &TrackingAllocator::Realloc;
  allocator_.free = &TrackingAllocator::Free;
  allocator_.user_data = this;
  previous_ = oemu_allocator_set(&allocator_);
}

TrackingAllocator::~TrackingAllocator() {
  // Restore before this object dies, otherwise the library would keep calling
  // into freed memory.
  oemu_allocator_set(previous_);
}

void *TrackingAllocator::Alloc(std::size_t size, void *user_data) {
  auto *self = static_cast<TrackingAllocator *>(user_data);
  void *ptr = std::malloc(size);
  if (ptr != nullptr) {
    ++self->alloc_count_;
    ++self->live_blocks_;
    self->bytes_requested_ += size;
  }
  return ptr;
}

void *TrackingAllocator::Realloc(void *ptr, std::size_t new_size, void *user_data) {
  auto *self = static_cast<TrackingAllocator *>(user_data);
  void *out = std::realloc(ptr, new_size);
  if (out != nullptr) {
    ++self->realloc_count_;
    self->bytes_requested_ += new_size;
    // realloc(nullptr, n) is an allocation and adds a live block; growing an
    // existing block does not change the count.
    if (ptr == nullptr) {
      ++self->alloc_count_;
      ++self->live_blocks_;
    }
  }
  return out;
}

void TrackingAllocator::Free(void *ptr, void *user_data) {
  auto *self = static_cast<TrackingAllocator *>(user_data);
  if (ptr != nullptr) {
    ++self->free_count_;
    if (self->live_blocks_ > 0) {
      --self->live_blocks_;
    }
  }
  std::free(ptr);
}

// --- FailingAllocator --------------------------------------------------------

FailingAllocator::FailingAllocator(std::size_t fail_on_call)
    : fail_on_call_(fail_on_call) {
  allocator_.alloc = &FailingAllocator::Alloc;
  allocator_.realloc = &FailingAllocator::Realloc;
  allocator_.free = &FailingAllocator::Free;
  allocator_.user_data = this;
  previous_ = oemu_allocator_set(&allocator_);
}

FailingAllocator::~FailingAllocator() { oemu_allocator_set(previous_); }

void FailingAllocator::set_fail_on_call(std::size_t fail_on_call) {
  fail_on_call_ = fail_on_call;
  call_count_ = 0;
  did_fail_ = false;
}

bool FailingAllocator::ShouldFail() {
  ++call_count_;
  if (fail_on_call_ != kNever && call_count_ == fail_on_call_) {
    did_fail_ = true;
    return true;
  }
  return false;
}

void *FailingAllocator::Alloc(std::size_t size, void *user_data) {
  auto *self = static_cast<FailingAllocator *>(user_data);
  if (self->ShouldFail()) {
    return nullptr;
  }
  return std::malloc(size);
}

void *FailingAllocator::Realloc(void *ptr, std::size_t new_size, void *user_data) {
  auto *self = static_cast<FailingAllocator *>(user_data);
  if (self->ShouldFail()) {
    // Returning nullptr without touching ptr matches realloc semantics: the
    // original block must stay valid, which is what the library relies on.
    return nullptr;
  }
  return std::realloc(ptr, new_size);
}

void FailingAllocator::Free(void *ptr, void *user_data) {
  (void)user_data;
  std::free(ptr);
}

}  // namespace oemu_test
