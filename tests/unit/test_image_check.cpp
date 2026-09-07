/*
 * Death tests for the Image loader's fatal contract (see image.c's
 * OEMU_REQUIRE). A half-built bus view is a programming error the loader
 * refuses to paper over: rather than return a status a caller might ignore
 * and lose a kernel, it aborts. This file pins that abort so the check cannot
 * be softened into a silent success path later.
 */
#include "oemu/image.h"
#include "oemu/memops.h"

#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "support/image_builder.h"

namespace {

namespace img = oemu_test::image;

/* A bus view whose write pointer was left null: a caller bug, not data. */
oemu_memops broken_bus(void) {
  oemu_memops ops{};
  ops.ctx = nullptr;
  ops.read = nullptr;
  ops.write = nullptr;
  return ops;
}

TEST(ImageChecked, AbortsOnABusViewWithNoWriteCallback) {
  const std::vector<uint8_t> file = img::file(0x1000U, img::park_text(1U), img::kFlagLe4K);
  oemu_image info{};
  oemu_memops ops = broken_bus();
  /* The child aborts inside the macro, so `st` is never assigned there; the
   * assignment is here to honour warn_unused_result in the parent. */
  oemu_status st = OEMU_OK;
  EXPECT_DEATH(
      st = oemu_image_load(&info, file.data(), file.size(), &ops, 0x40000000ULL, 0x100000ULL),
      "half-built bus view");
  EXPECT_EQ(OEMU_OK, st);
}

}  // namespace
