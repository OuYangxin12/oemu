/*
 * Death tests for the PL011 model's fatal contract (pl011.c OEMU_REQUIRE).
 * A device handle must never be null: the bus callbacks are wired at init
 * and a null uart is a wiring bug, not a runtime condition to recover from.
 * These aborts are the point -- they pin that a mistake is loud.
 */
#include <cstddef>

#include <gtest/gtest.h>

#include "oemu/pl011.h"

namespace {

TEST(Pl011Checked, AbortsWhenInitGivenANullDevice) {
  EXPECT_DEATH(oemu_pl011_init(nullptr, nullptr, nullptr), "NULL oemu_pl011");
}

TEST(Pl011Checked, AbortsWhenPumpGivenANullDevice) {
  EXPECT_DEATH(oemu_pl011_pump(nullptr), "NULL oemu_pl011");
}

TEST(Pl011Checked, AbortsWhenIrqLevelGivenANullDevice) {
  EXPECT_DEATH(oemu_pl011_irq_level(nullptr), "NULL oemu_pl011");
}

}  // namespace
