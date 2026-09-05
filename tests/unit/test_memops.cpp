/*
 * Tests for the bus seam (include/oemu/memops.h): the two views that exist
 * today, the environment view, and the executor entry points that run over
 * them.
 *
 * Two things are worth proving here and nowhere else:
 *
 *   1. A view is behaviour-free. Every assertion below pairs a call through
 *      the view with the same call on the owning module and demands the same
 *      answer, including the refusals -- a view that "helpfully" repaired a
 *      NULL context or a misaligned fetch would make the seam a second,
 *      divergent implementation of the bus.
 *   2. The interpreter no longer knows what it is running on. The same
 *      hand-assembled program is driven through an oemu_aspace, complete with
 *      an MMIO device as a store target, which is exactly what full-system
 *      mode needs and what oemu_memory could never offer.
 */
#include "oemu/aspace.h"
#include "oemu/device.h"
#include "oemu/exec.h"
#include "oemu/memops.h"
#include "oemu/memory.h"
#include "oemu/regs.h"
#include "oemu/sysenv.h"

#include <array>
#include <cstdio>
#include <initializer_list>
#include <string>

#include <gtest/gtest.h>

#include "exec/exec_internal.h"
#include "support/tracking_allocator.h"

namespace {

constexpr uint64_t kRam = UINT64_C(0x40000000);
constexpr uint64_t kDev = UINT64_C(0x09000000); /* PL011-shaped address */
constexpr uint64_t kFlat = UINT64_C(0x100000);  /* oemu_memory test region */

/* --- memory view ---------------------------------------------------------------- */

class MemopsMemoryView : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(oemu_memory_init(&mem_, 4U), OEMU_OK);
    ASSERT_EQ(oemu_memory_map(&mem_, kFlat, 0x1000U, OEMU_PERM_ALL), OEMU_OK);
    ASSERT_EQ(
        oemu_memory_map(&mem_, kFlat + 0x8000U, 0x1000U, OEMU_PERM_READ | OEMU_PERM_WRITE),
        OEMU_OK);
    bus_ = oemu_memory_memops(&mem_);
  }
  void TearDown() override {
    oemu_memory_dispose(&mem_);
    EXPECT_FALSE(tracker_.has_leaks());
  }

  oemu_memory mem_{};
  oemu_memops bus_{};
  oemu_test::TrackingAllocator tracker_;
};

TEST_F(MemopsMemoryView, CarriesTheOwnerAndEveryCallback) {
  EXPECT_EQ(bus_.ctx, &mem_);
  EXPECT_NE(bus_.fetch32, nullptr);
  EXPECT_NE(bus_.read, nullptr);
  EXPECT_NE(bus_.write, nullptr);
  EXPECT_NE(bus_.validate, nullptr);
}

TEST_F(MemopsMemoryView, ReadsAndWritesTheSameMemoryTheOwnerSees) {
  ASSERT_EQ(bus_.write(bus_.ctx, kFlat, OEMU_MEM_DWORD, UINT64_C(0x1122334455667788)), OEMU_OK);
  uint64_t direct = 0U;
  EXPECT_EQ(oemu_memory_read(&mem_, kFlat, OEMU_MEM_DWORD, false, &direct), OEMU_OK);
  EXPECT_EQ(direct, UINT64_C(0x1122334455667788));

  ASSERT_EQ(oemu_memory_write(&mem_, kFlat + 8U, OEMU_MEM_WORD, UINT64_C(0xFFFFFF80)), OEMU_OK);
  uint64_t through_view = 0U;
  EXPECT_EQ(bus_.read(bus_.ctx, kFlat + 8U, OEMU_MEM_WORD, false, &through_view), OEMU_OK);
  EXPECT_EQ(through_view, UINT64_C(0xFFFFFF80));
  /* Sign extension is the owner's, not the view's. */
  EXPECT_EQ(bus_.read(bus_.ctx, kFlat + 8U, OEMU_MEM_WORD, true, &through_view), OEMU_OK);
  EXPECT_EQ(through_view, UINT64_C(0xFFFFFFFFFFFFFF80));
}

TEST_F(MemopsMemoryView, ValidateTakesByteCountsAndAgreesWithTheOwner) {
  EXPECT_EQ(bus_.validate(bus_.ctx, kFlat, 8U, OEMU_PERM_READ),
            oemu_memory_validate(&mem_, kFlat, 8U, OEMU_PERM_READ));
  EXPECT_EQ(bus_.validate(bus_.ctx, kFlat, 8U, OEMU_PERM_READ), OEMU_OK);
  /* One byte past the region is a fault through both paths. */
  EXPECT_EQ(bus_.validate(bus_.ctx, kFlat + 0x1000U - 4U, 8U, OEMU_PERM_READ),
            oemu_memory_validate(&mem_, kFlat + 0x1000U - 4U, 8U, OEMU_PERM_READ));
  EXPECT_EQ(bus_.validate(bus_.ctx, kFlat + 0x1000U - 4U, 8U, OEMU_PERM_READ), OEMU_ERR_FAULT);
  /* The non-executable region refuses EXEC through the view too. */
  EXPECT_EQ(bus_.validate(bus_.ctx, kFlat + 0x8000U, 4U, OEMU_PERM_EXEC), OEMU_ERR_FAULT);
}

TEST_F(MemopsMemoryView, Fetch32KeepsTheOwnersPermissionAndAlignmentRules) {
  ASSERT_EQ(oemu_memory_write(&mem_, kFlat, OEMU_MEM_WORD, UINT64_C(0xd4200000)), OEMU_OK);
  uint32_t word = 0U;
  EXPECT_EQ(bus_.fetch32(bus_.ctx, kFlat, &word), OEMU_OK);
  EXPECT_EQ(word, UINT32_C(0xd4200000));
  EXPECT_EQ(bus_.fetch32(bus_.ctx, kFlat + 2U, &word), OEMU_ERR_FAULT);      /* misaligned */
  EXPECT_EQ(bus_.fetch32(bus_.ctx, kFlat + 0x8000U, &word), OEMU_ERR_FAULT); /* no EXEC */
}

TEST_F(MemopsMemoryView, RefusesANullContextExactlyLikeTheEntryPoints) {
  /* Each entry point has its own answer for a NULL model -- FAULT where the
   * region lookup simply finds nothing, INVALID_ARG where the argument check
   * comes first -- and the view must reproduce whichever it is, not a tidier
   * story of its own. */
  const oemu_memops orphan = oemu_memory_memops(nullptr);
  uint64_t value = 0U;
  uint64_t direct_value = 0U;
  uint32_t word = 0U;
  uint32_t direct_word = 0U;
  EXPECT_EQ(orphan.ctx, nullptr);
  EXPECT_EQ(orphan.read(orphan.ctx, kFlat, OEMU_MEM_BYTE, false, &value),
            oemu_memory_read(nullptr, kFlat, OEMU_MEM_BYTE, false, &direct_value));
  EXPECT_EQ(orphan.write(orphan.ctx, kFlat, OEMU_MEM_BYTE, 0U),
            oemu_memory_write(nullptr, kFlat, OEMU_MEM_BYTE, 0U));
  EXPECT_EQ(orphan.validate(orphan.ctx, kFlat, 1U, OEMU_PERM_READ),
            oemu_memory_validate(nullptr, kFlat, 1U, OEMU_PERM_READ));
  EXPECT_EQ(orphan.fetch32(orphan.ctx, kFlat, &word),
            oemu_memory_fetch32(nullptr, kFlat, &direct_word));
  /* And none of those answers is a success. */
  EXPECT_NE(orphan.read(orphan.ctx, kFlat, OEMU_MEM_BYTE, false, &value), OEMU_OK);
  EXPECT_NE(orphan.write(orphan.ctx, kFlat, OEMU_MEM_BYTE, 0U), OEMU_OK);
  EXPECT_NE(orphan.validate(orphan.ctx, kFlat, 1U, OEMU_PERM_READ), OEMU_OK);
  EXPECT_NE(orphan.fetch32(orphan.ctx, kFlat, &word), OEMU_OK);
}

/* --- address-space view --------------------------------------------------------- */

/* Records what the bus asked for, so a test can prove the routing rather than
 * the device. */
struct FakeDevice {
  uint64_t read_value = 0U;
  uint64_t last_offset = 0U;
  oemu_mem_size last_size = OEMU_MEM_BYTE;
  uint64_t last_write = 0U;
  int reads = 0;
  int writes = 0;

  static oemu_status Read(void *ctx, uint64_t offset, oemu_mem_size size, uint64_t *value_out) {
    auto *dev = static_cast<FakeDevice *>(ctx);
    ++dev->reads;
    dev->last_offset = offset;
    dev->last_size = size;
    *value_out = dev->read_value;
    return OEMU_OK;
  }
  static oemu_status Write(void *ctx, uint64_t offset, oemu_mem_size size, uint64_t value) {
    auto *dev = static_cast<FakeDevice *>(ctx);
    ++dev->writes;
    dev->last_offset = offset;
    dev->last_size = size;
    dev->last_write = value;
    return OEMU_OK;
  }
};

class MemopsAspaceView : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(oemu_aspace_init(&as_, 4U), OEMU_OK);
    ASSERT_EQ(oemu_aspace_map_ram(&as_, kRam, 0x1000U, OEMU_PERM_ALL, nullptr), OEMU_OK);
    ops_.ctx = &dev_;
    ops_.read = &FakeDevice::Read;
    ops_.write = &FakeDevice::Write;
    ASSERT_EQ(oemu_aspace_attach_device(&as_, kDev, 0x1000U, &ops_), OEMU_OK);
    bus_ = oemu_aspace_memops(&as_);
  }
  void TearDown() override {
    oemu_aspace_dispose(&as_);
    EXPECT_FALSE(tracker_.has_leaks());
  }

  oemu_aspace as_{};
  FakeDevice dev_{};
  oemu_device_ops ops_{};
  oemu_memops bus_{};
  oemu_test::TrackingAllocator tracker_;
};

TEST_F(MemopsAspaceView, RamTrafficMatchesTheOwner) {
  ASSERT_EQ(bus_.write(bus_.ctx, kRam + 0x40U, OEMU_MEM_DWORD, UINT64_C(0xDEADBEEFCAFEF00D)),
            OEMU_OK);
  uint64_t direct = 0U;
  EXPECT_EQ(oemu_aspace_read(&as_, kRam + 0x40U, OEMU_MEM_DWORD, false, &direct), OEMU_OK);
  EXPECT_EQ(direct, UINT64_C(0xDEADBEEFCAFEF00D));
  EXPECT_EQ(bus_.validate(bus_.ctx, kRam, 16U, OEMU_PERM_READ | OEMU_PERM_WRITE), OEMU_OK);
  EXPECT_EQ(bus_.validate(bus_.ctx, kRam - 4U, 8U, OEMU_PERM_READ), OEMU_ERR_FAULT);
}

TEST_F(MemopsAspaceView, DeviceTrafficReachesTheDeviceThroughTheView) {
  dev_.read_value = UINT64_C(0x5A);
  uint64_t value = 0U;
  ASSERT_EQ(bus_.read(bus_.ctx, kDev + 0x18U, OEMU_MEM_WORD, false, &value), OEMU_OK);
  EXPECT_EQ(value, UINT64_C(0x5A));
  EXPECT_EQ(dev_.reads, 1);
  EXPECT_EQ(dev_.last_offset, UINT64_C(0x18)); /* device-relative, as always */

  ASSERT_EQ(bus_.write(bus_.ctx, kDev, OEMU_MEM_WORD, UINT64_C(0x41)), OEMU_OK);
  EXPECT_EQ(dev_.writes, 1);
  EXPECT_EQ(dev_.last_write, UINT64_C(0x41));
  EXPECT_EQ(dev_.last_size, OEMU_MEM_WORD);

  /* A device is never a code source, and the view does not soften that. */
  uint32_t word = 0U;
  EXPECT_EQ(bus_.fetch32(bus_.ctx, kDev, &word), OEMU_ERR_FAULT);
  EXPECT_EQ(dev_.reads, 1); /* the refusal happened before the device was asked */
}

TEST_F(MemopsAspaceView, RefusesANullContextExactlyLikeTheEntryPoints) {
  const oemu_memops orphan = oemu_aspace_memops(nullptr);
  uint64_t value = 0U;
  uint32_t word = 0U;
  EXPECT_EQ(orphan.read(orphan.ctx, kRam, OEMU_MEM_BYTE, false, &value), OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(orphan.write(orphan.ctx, kRam, OEMU_MEM_BYTE, 0U), OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(orphan.validate(orphan.ctx, kRam, 1U, OEMU_PERM_READ), OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(orphan.fetch32(orphan.ctx, kRam, &word), OEMU_ERR_INVALID_ARG);
}

/* --- environment view ------------------------------------------------------------ */

class EnvopsSysenv : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(oemu_memory_init(&mem_, 4U), OEMU_OK);
    ASSERT_EQ(oemu_memory_map(&mem_, kFlat, 0x1000U, OEMU_PERM_READ | OEMU_PERM_WRITE),
              OEMU_OK);
    bus_ = oemu_memory_memops(&mem_);
    out_ = std::tmpfile();
    ASSERT_NE(out_, nullptr);
    oemu_sysenv_init(&env_, out_);
    envops_ = oemu_sysenv_envops(&env_);
  }
  void TearDown() override {
    if (out_ != nullptr) {
      std::fclose(out_);
    }
    oemu_memory_dispose(&mem_);
    EXPECT_FALSE(tracker_.has_leaks());
  }

  std::string captured() {
    std::fflush(out_);
    std::rewind(out_);
    std::string text;
    int c = std::fgetc(out_);
    while (c != EOF) {
      text.push_back(static_cast<char>(c));
      c = std::fgetc(out_);
    }
    return text;
  }

  oemu_memory mem_{};
  oemu_memops bus_{};
  oemu_sysenv env_{};
  oemu_env_ops envops_{};
  FILE *out_ = nullptr;
  oemu_test::TrackingAllocator tracker_;
};

TEST_F(EnvopsSysenv, CarriesTheEnvironmentAndBothCallbacks) {
  EXPECT_EQ(envops_.ctx, &env_);
  EXPECT_NE(envops_.syscall, nullptr);
  EXPECT_NE(envops_.halted, nullptr);
}

TEST_F(EnvopsSysenv, WriteReachesGuestMemoryThroughTheSuppliedBus) {
  const std::string text = "seam";
  ASSERT_EQ(oemu_memory_write_bytes(
                &mem_, kFlat, reinterpret_cast<const uint8_t *>(text.data()), text.size()),
            OEMU_OK);
  const std::array<uint64_t, 6> args{1U, kFlat, text.size(), 0U, 0U, 0U};
  EXPECT_EQ(envops_.syscall(envops_.ctx, &bus_, OEMU_SYS_WRITE, args.data()),
            static_cast<int64_t>(text.size()));
  EXPECT_EQ(captured(), text);
}

TEST_F(EnvopsSysenv, WriteWithoutABusIsEfaultRatherThanACrash) {
  const std::array<uint64_t, 6> args{1U, kFlat, 4U, 0U, 0U, 0U};
  EXPECT_EQ(envops_.syscall(envops_.ctx, nullptr, OEMU_SYS_WRITE, args.data()),
            -static_cast<int64_t>(OEMU_EFAULT));
}

TEST_F(EnvopsSysenv, ClockGettimeWithoutABusIsEfaultRatherThanACrash) {
  const std::array<uint64_t, 6> args{OEMU_CLOCK_MONOTONIC, kFlat, 0U, 0U, 0U, 0U};
  EXPECT_EQ(envops_.syscall(envops_.ctx, nullptr, OEMU_SYS_CLOCK_GETTIME, args.data()),
            -static_cast<int64_t>(OEMU_EFAULT));
}

TEST_F(EnvopsSysenv, HaltedFollowsTheExitSyscall) {
  EXPECT_FALSE(envops_.halted(envops_.ctx));
  const std::array<uint64_t, 6> args{7U, 0U, 0U, 0U, 0U, 0U};
  EXPECT_EQ(envops_.syscall(envops_.ctx, &bus_, OEMU_SYS_EXIT, args.data()), 0);
  EXPECT_TRUE(envops_.halted(envops_.ctx));
  EXPECT_EQ(oemu_sysenv_exit_code(&env_), 7);
}

/* --- the executor over an address space ------------------------------------------ */

/*
 * The point of the seam: this fixture runs real programs with no oemu_memory
 * anywhere, on the same kind of bus a machine will hand the vCPU in M2.
 */
class ExecBus : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(oemu_aspace_init(&as_, 4U), OEMU_OK);
    ASSERT_EQ(oemu_aspace_map_ram(&as_, kRam, 0x1000U, OEMU_PERM_ALL, nullptr), OEMU_OK);
    ops_.ctx = &dev_;
    ops_.read = &FakeDevice::Read;
    ops_.write = &FakeDevice::Write;
    ASSERT_EQ(oemu_aspace_attach_device(&as_, kDev, 0x1000U, &ops_), OEMU_OK);
    bus_ = oemu_aspace_memops(&as_);
    out_ = std::tmpfile();
    ASSERT_NE(out_, nullptr);
    oemu_sysenv_init(&env_, out_);
    envops_ = oemu_sysenv_envops(&env_);
    ASSERT_EQ(oemu_cpu_init(&cpu_, kRam, kRam + 0x800U), OEMU_OK);
    allocations_at_setup_ = tracker_.alloc_count();
  }
  void TearDown() override {
    if (out_ != nullptr) {
      std::fclose(out_);
    }
    oemu_aspace_dispose(&as_);
    EXPECT_FALSE(tracker_.has_leaks());
    /* Running over the seam must stay allocation-free, like the old path. */
    EXPECT_EQ(tracker_.alloc_count(), allocations_at_setup_);
  }

  void program(std::initializer_list<uint32_t> words) {
    uint64_t addr = kRam;
    for (const uint32_t w : words) {
      ASSERT_EQ(oemu_aspace_write(&as_, addr, OEMU_MEM_WORD, w), OEMU_OK);
      addr += OEMU_INSN_SIZE;
    }
    oemu_regs_set_pc(&cpu_.regs, kRam);
  }
  void set_x(unsigned n, uint64_t value) {
    oemu_regs_write(&cpu_.regs, n, OEMU_REG_W64, value);
  }
  uint64_t x(unsigned n) { return oemu_regs_read(&cpu_.regs, n, OEMU_REG_W64); }
  uint64_t pc() { return oemu_regs_pc(&cpu_.regs); }

  oemu_aspace as_{};
  FakeDevice dev_{};
  oemu_device_ops ops_{};
  oemu_memops bus_{};
  oemu_sysenv env_{};
  oemu_env_ops envops_{};
  oemu_cpu cpu_{};
  FILE *out_ = nullptr;
  oemu_test::TrackingAllocator tracker_;
  std::size_t allocations_at_setup_ = 0U;
};

TEST_F(ExecBus, RunsAProgramOutOfPhysicalRam) {
  program({0xd2800160U, /* mov x0,#11 */
           0x91000400U, /* add x0,x0,#1 */
           0xd4200000U /* brk #0 */});
  uint64_t done = 0U;
  EXPECT_EQ(oemu_exec_run_bus(&cpu_, &bus_, &envops_, 8U, &done), OEMU_ERR_FAULT);
  EXPECT_EQ(done, 2U);
  EXPECT_EQ(x(0), 12U);
  EXPECT_EQ(pc(), kRam + 8U); /* the BRK itself committed nothing */
}

TEST_F(ExecBus, StoresAndLoadsReachAnMmioDevice) {
  program({0xb9000041U, /* str w1,[x2] */
           0xb9400043U /* ldr w3,[x2] */});
  set_x(1, UINT64_C(0x41));
  set_x(2, kDev);
  dev_.read_value = UINT64_C(0x5A);

  ASSERT_EQ(oemu_exec_step_bus(&cpu_, &bus_, &envops_, nullptr), OEMU_OK);
  EXPECT_EQ(dev_.writes, 1);
  EXPECT_EQ(dev_.last_write, UINT64_C(0x41));
  EXPECT_EQ(dev_.last_offset, 0U);

  ASSERT_EQ(oemu_exec_step_bus(&cpu_, &bus_, &envops_, nullptr), OEMU_OK);
  EXPECT_EQ(dev_.reads, 1);
  EXPECT_EQ(x(3), UINT64_C(0x5A));
}

TEST_F(ExecBus, SvcDispatchesThroughTheEnvironmentView) {
  program({0xd2800160U, /* mov x0,#11 */
           0xd2800ba8U, /* mov x8,#93 (exit) */
           0xd4000001U /* svc #0 */});
  uint64_t done = 0U;
  EXPECT_EQ(oemu_exec_run_bus(&cpu_, &bus_, &envops_, 16U, &done), OEMU_OK);
  EXPECT_EQ(done, 3U);
  EXPECT_TRUE(oemu_sysenv_exited(&env_));
  EXPECT_EQ(oemu_sysenv_exit_code(&env_), 11);
}

TEST_F(ExecBus, SvcWithoutAnEnvironmentIsUnsupportedAndPrecise) {
  program({0xd4000001U}); /* svc #0 */
  EXPECT_EQ(oemu_exec_step_bus(&cpu_, &bus_, nullptr, nullptr), OEMU_ERR_UNSUPPORTED);
  EXPECT_EQ(pc(), kRam);
}

TEST_F(ExecBus, RunWithoutAnEnvironmentSpendsTheBudget) {
  program({0xd503201fU, /* nop */ 0xd503201fU, 0xd503201fU, 0xd503201fU});
  uint64_t done = 0U;
  EXPECT_EQ(oemu_exec_run_bus(&cpu_, &bus_, nullptr, 3U, &done), OEMU_ERR_TIMEOUT);
  EXPECT_EQ(done, 3U);
  EXPECT_EQ(pc(), kRam + 12U);
}

TEST_F(ExecBus, AFaultingAccessCommitsNothing) {
  program({0xf8410c20U}); /* ldr x0,[x1,#16]! -- pre-index, so writeback too */
  set_x(0, UINT64_C(0xAAAA));
  set_x(1, UINT64_C(0x80000000)); /* unmapped */
  EXPECT_EQ(oemu_exec_step_bus(&cpu_, &bus_, &envops_, nullptr), OEMU_ERR_FAULT);
  EXPECT_EQ(x(0), UINT64_C(0xAAAA));
  EXPECT_EQ(x(1), UINT64_C(0x80000000));
  EXPECT_EQ(pc(), kRam);
}

TEST_F(ExecBus, AnExclusiveLoadFaultsWithoutTakingAReservation) {
  program({0xc85ffc20U});         /* ldaxr x0,[x1] */
  set_x(1, UINT64_C(0x80000000)); /* unmapped */
  EXPECT_EQ(oemu_exec_step_bus(&cpu_, &bus_, &envops_, nullptr), OEMU_ERR_FAULT);
  EXPECT_FALSE(cpu_.monitor_valid);
  EXPECT_EQ(pc(), kRam);
}

TEST_F(ExecBus, StepBusRejectsNullCpuAndBus) {
  oemu_insn insn{};
  EXPECT_EQ(oemu_exec_step_bus(nullptr, &bus_, &envops_, &insn), OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(oemu_exec_step_bus(&cpu_, nullptr, &envops_, &insn), OEMU_ERR_INVALID_ARG);
}

TEST_F(ExecBus, DispatchBusRejectsAHalfBuiltView) {
  program({0xd503201fU}); /* nop */
  oemu_insn insn{};
  ASSERT_EQ(oemu_decode(UINT32_C(0xd503201f), kRam, &insn), OEMU_OK);

  EXPECT_EQ(oemu_exec_internal_dispatch_bus(&cpu_, nullptr, &envops_, &insn),
            OEMU_ERR_INVALID_ARG);
  for (int missing = 0; missing < 4; missing++) {
    oemu_memops broken = bus_;
    switch (missing) {
      case 0:
        broken.fetch32 = nullptr;
        break;
      case 1:
        broken.read = nullptr;
        break;
      case 2:
        broken.write = nullptr;
        break;
      default:
        broken.validate = nullptr;
        break;
    }
    EXPECT_EQ(oemu_exec_internal_dispatch_bus(&cpu_, &broken, &envops_, &insn),
              OEMU_ERR_INVALID_ARG)
        << "callback " << missing;
  }
  /* The complete view still runs the same instruction. */
  EXPECT_EQ(oemu_exec_internal_dispatch_bus(&cpu_, &bus_, &envops_, &insn), OEMU_OK);
  EXPECT_EQ(pc(), kRam + 4U);
}

TEST_F(ExecBus, DispatchBusRejectsNullCpuAndInstruction) {
  oemu_insn insn{};
  ASSERT_EQ(oemu_decode(UINT32_C(0xd503201f), kRam, &insn), OEMU_OK);
  EXPECT_EQ(oemu_exec_internal_dispatch_bus(nullptr, &bus_, &envops_, &insn),
            OEMU_ERR_INVALID_ARG);
  EXPECT_EQ(oemu_exec_internal_dispatch_bus(&cpu_, &bus_, &envops_, nullptr),
            OEMU_ERR_INVALID_ARG);
  const oemu_insn unknown{};
  EXPECT_EQ(oemu_exec_internal_dispatch_bus(&cpu_, &bus_, &envops_, &unknown),
            OEMU_ERR_INVALID_ARG);
}

/* --- the old entry points are the same machine ------------------------------------ */

/* Runs one program twice, once through oemu_exec_run and once through
 * oemu_exec_run_bus over a view of the very same oemu_memory, and demands
 * identical results: the wrappers may not have grown behaviour of their own. */
TEST(ExecBusParity, WrapperAndSeamAgree) {
  oemu_test::TrackingAllocator tracker;
  const std::array<uint32_t, 3> words{0xd2800160U, 0xd2800ba8U, 0xd4000001U};

  uint64_t done[2] = {0U, 0U};
  int exit_code[2] = {-1, -1};
  oemu_status st[2] = {OEMU_ERR_FAULT, OEMU_ERR_FAULT};

  for (int through_seam = 0; through_seam < 2; through_seam++) {
    oemu_memory mem{};
    ASSERT_EQ(oemu_memory_init(&mem, 2U), OEMU_OK);
    ASSERT_EQ(oemu_memory_map(&mem, kFlat, 0x1000U, OEMU_PERM_ALL), OEMU_OK);
    uint64_t addr = kFlat;
    for (const uint32_t w : words) {
      ASSERT_EQ(oemu_memory_write(&mem, addr, OEMU_MEM_WORD, w), OEMU_OK);
      addr += OEMU_INSN_SIZE;
    }
    FILE *out = std::tmpfile();
    ASSERT_NE(out, nullptr);
    oemu_sysenv env{};
    oemu_sysenv_init(&env, out);
    oemu_cpu cpu{};
    ASSERT_EQ(oemu_cpu_init(&cpu, kFlat, kFlat + 0x800U), OEMU_OK);

    if (through_seam == 0) {
      st[through_seam] = oemu_exec_run(&cpu, &mem, &env, 16U, &done[through_seam]);
    } else {
      const oemu_memops bus = oemu_memory_memops(&mem);
      const oemu_env_ops envops = oemu_sysenv_envops(&env);
      st[through_seam] = oemu_exec_run_bus(&cpu, &bus, &envops, 16U, &done[through_seam]);
    }
    exit_code[through_seam] = oemu_sysenv_exit_code(&env);

    std::fclose(out);
    oemu_memory_dispose(&mem);
  }

  EXPECT_EQ(st[0], OEMU_OK);
  EXPECT_EQ(st[0], st[1]);
  EXPECT_EQ(done[0], done[1]);
  EXPECT_EQ(exit_code[0], exit_code[1]);
  EXPECT_EQ(exit_code[1], 11);
  EXPECT_FALSE(tracker.has_leaks());
}

} /* namespace */
