/*
 * Tests for the machine: construction's two allocations and their failure
 * paths, and the sticky lifecycle events. Acting on an event belongs to a
 * later phase; these tests pin down that the machine records it faithfully.
 */
#include "oemu/machine.h"

#include <gtest/gtest.h>

#include "support/tracking_allocator.h"

namespace {

constexpr uint64_t kRamBase = UINT64_C(0x40000000);
constexpr uint64_t kRamSize = UINT64_C(0x10000);

class MachineTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(oemu_machine_init(&machine_, kRamBase, kRamSize, 4U), OEMU_OK);
  }
  void TearDown() override {
    oemu_machine_dispose(&machine_);
    EXPECT_FALSE(tracker_.has_leaks());
  }

  oemu_machine machine_{};
  oemu_test::TrackingAllocator tracker_;
};

TEST(MachineInit, RejectsBadArguments) {
  EXPECT_EQ(oemu_machine_init(nullptr, kRamBase, kRamSize, 4U), OEMU_ERR_INVALID_ARG);
  oemu_machine machine{};
  EXPECT_EQ(oemu_machine_init(&machine, kRamBase, 0U, 4U), OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(oemu_machine_init(&machine, kRamBase, kRamSize, 0U), OEMU_ERR_INVALID_ARG);
}

TEST(MachineInit, TableOomLeavesMachineZeroedAndSafe) {
  const oemu_test::FailingAllocator failing(1U); /* the region table */
  oemu_machine machine{};
  machine.event = OEMU_MACHINE_EVENT_POWERDOWN; /* stale bytes must not survive */
  EXPECT_EQ(oemu_machine_init(&machine, kRamBase, kRamSize, 4U), OEMU_ERR_NO_MEMORY);
  EXPECT_TRUE(failing.did_fail());
  EXPECT_EQ(machine.event, OEMU_MACHINE_EVENT_NONE);
  EXPECT_EQ(machine.aspace.regions, nullptr);
  oemu_machine_dispose(&machine); /* dispose after a failed init is safe */
}

TEST(MachineInit, RamOomDisposesTheTableItAlreadyTook) {
  /* Call 1 is the region table, call 2 the RAM block. The table came from the
   * failing pool, so only dispose can hand it back -- under ASan a missed
   * unwind here is a leak report, which is what makes this test bite. */
  const oemu_test::FailingAllocator failing(2U);
  oemu_machine machine{};
  EXPECT_EQ(oemu_machine_init(&machine, kRamBase, kRamSize, 4U), OEMU_ERR_NO_MEMORY);
  EXPECT_TRUE(failing.did_fail());
  EXPECT_EQ(machine.aspace.regions, nullptr); /* half-built state unwound */
  EXPECT_EQ(machine.aspace.region_count, 0U);
  oemu_machine_dispose(&machine);
}

TEST(MachineInit, RamRangeWrapIsRejected) {
  oemu_machine machine{};
  EXPECT_EQ(oemu_machine_init(&machine, UINT64_MAX - 1024U, 4096U, 4U), OEMU_ERR_OVERFLOW);
  EXPECT_EQ(machine.aspace.regions, nullptr);
  oemu_machine_dispose(&machine);
}

TEST_F(MachineTest, ExactlyTwoAllocationsPerMachine) {
  EXPECT_EQ(tracker_.alloc_count(), 2U); /* the region table and the RAM block */
}

TEST_F(MachineTest, RamIsMappedReadWriteExecuteAtTheGivenBase) {
  /* The loader writes through the bus, so RAM needs no privileged back door. */
  ASSERT_EQ(oemu_aspace_write(&machine_.aspace, kRamBase, OEMU_MEM_WORD, 0xD65F03C0U), OEMU_OK);
  uint32_t word = 0;
  ASSERT_EQ(oemu_aspace_fetch32(&machine_.aspace, kRamBase, &word), OEMU_OK);
  EXPECT_EQ(word, 0xD65F03C0U); /* RET, fetched as code: the region is executable */
  /* Just outside the RAM window: the bus does not invent coverage. */
  uint64_t value = 0;
  EXPECT_EQ(
      oemu_aspace_read(&machine_.aspace, kRamBase + kRamSize, OEMU_MEM_WORD, false, &value),
      OEMU_ERR_FAULT);
}

TEST_F(MachineTest, DevicesComposeOntoTheMachineBus) {
  /* The point of the machine owning the bus: a device model plugs in here,
   * using a region slot the init left free. */
  struct Reg {
    uint64_t value = 0;
  } reg;
  oemu_device_ops ops{};
  ops.ctx = &reg;
  ops.read = [](void *ctx, uint64_t, oemu_mem_size, uint64_t *out) {
    *out = static_cast<Reg *>(ctx)->value;
    return OEMU_OK;
  };
  ops.write = [](void *ctx, uint64_t, oemu_mem_size, uint64_t value) {
    static_cast<Reg *>(ctx)->value = value;
    return OEMU_OK;
  };
  ASSERT_EQ(oemu_aspace_attach_device(&machine_.aspace, 0x09000000U, 0x1000U, &ops), OEMU_OK);
  ASSERT_EQ(oemu_aspace_write(&machine_.aspace, 0x09000004U, OEMU_MEM_WORD, 0xA5U), OEMU_OK);
  EXPECT_EQ(reg.value, 0xA5U);
  uint64_t value = 0;
  ASSERT_EQ(oemu_aspace_read(&machine_.aspace, 0x09000004U, OEMU_MEM_WORD, false, &value),
            OEMU_OK);
  EXPECT_EQ(value, 0xA5U);
  EXPECT_EQ(machine_.aspace.region_count, 2U);
}

/* ---- lifecycle events ------------------------------------------------------ */

TEST_F(MachineTest, StartsRunningWithNoEvent) {
  EXPECT_EQ(oemu_machine_event_peek(&machine_), OEMU_MACHINE_EVENT_NONE);
  EXPECT_EQ(machine_.exit_code, 0);
}

TEST_F(MachineTest, PoweroffIsStickyAndCarriesTheExitCode) {
  oemu_machine_poweroff(&machine_, 3);
  EXPECT_EQ(oemu_machine_event_peek(&machine_), OEMU_MACHINE_EVENT_POWERDOWN);
  EXPECT_EQ(machine_.exit_code, 3);
  oemu_machine_poweroff(&machine_, 7); /* re-raising just updates the wish */
  EXPECT_EQ(oemu_machine_event_peek(&machine_), OEMU_MACHINE_EVENT_POWERDOWN);
  EXPECT_EQ(machine_.exit_code, 7);
}

TEST_F(MachineTest, ResetOverwritesThePendingEvent) {
  oemu_machine_poweroff(&machine_, 0);
  oemu_machine_reset(&machine_);
  EXPECT_EQ(oemu_machine_event_peek(&machine_), OEMU_MACHINE_EVENT_RESET);
}

TEST_F(MachineTest, DisposeClearsThePendingEvent) {
  oemu_machine_reset(&machine_);
  oemu_machine_dispose(&machine_);
  EXPECT_EQ(oemu_machine_event_peek(&machine_), OEMU_MACHINE_EVENT_NONE);
  ASSERT_EQ(oemu_machine_init(&machine_, kRamBase, kRamSize, 4U),
            OEMU_OK); /* dispose must leave it re-initable */
}

TEST(MachineNullSafety, NullArgumentsDoNotCrash) {
  oemu_machine_dispose(nullptr);
  oemu_machine_poweroff(nullptr, 1);
  oemu_machine_reset(nullptr);
  EXPECT_EQ(oemu_machine_event_peek(nullptr), OEMU_MACHINE_EVENT_NONE);
}

TEST(MachineDispose, ZeroedAndDoubleDisposeAreSafe) {
  oemu_test::TrackingAllocator tracker;
  oemu_machine machine{};
  oemu_machine_dispose(&machine); /* never initialised */
  ASSERT_EQ(oemu_machine_init(&machine, kRamBase, kRamSize, 4U), OEMU_OK);
  oemu_machine_dispose(&machine);
  oemu_machine_dispose(&machine);
  EXPECT_FALSE(tracker.has_leaks());
}

TEST(MachineDispose, RamIsFreedEvenWithPendingEvent) {
  oemu_test::TrackingAllocator tracker;
  oemu_machine machine{};
  ASSERT_EQ(oemu_machine_init(&machine, kRamBase, kRamSize, 4U), OEMU_OK);
  oemu_machine_poweroff(&machine, 1);
  oemu_machine_dispose(&machine);
  EXPECT_FALSE(tracker.has_leaks());
}

}  // namespace
