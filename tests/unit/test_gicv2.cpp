/*
 * Black-box tests for the GICv2 model (include/oemu/gicv2.h).
 *
 * Every case drives the controller the way Linux's irq-gic driver does: attach
 * the two memory regions to a machine's address space and talk to them through
 * the bus. Nothing pokes the struct directly, so the observable contract -- the
 * GIC-400 peripheral identity the driver matches on, RAZ/WI for unmodelled
 * offsets (never a fault), the acknowledge/EOI handshake on the CPU interface,
 * and the priority scan -- is what is under test. Expected values are the ones
 * qemu-system-aarch64's `arm_gic` presents (the DT compatible is
 * "arm,cortex-a15-gic"): GICD_PIDR2 == 0x2B, a 64-line (two-group) TYPER.
 */
#include "oemu/aspace.h"
#include "oemu/machine.h"
#include "oemu/status.h"

#include <cstddef>
#include <cstdint>

#include <gtest/gtest.h>

#include "oemu/gicv2.h"

namespace {

constexpr uint64_t kRamBase = 0x40000000ULL;
constexpr uint64_t kRamSize = 0x00010000ULL;
constexpr uint64_t kDist = 0x08000000ULL;
constexpr uint64_t kCpu = 0x08010000ULL;
constexpr uint64_t kRegion = 0x00010000ULL;

/* Distributor offsets (GICv2 numbering, as the driver and QEMU use). */
constexpr uint64_t DIST_CTL = 0x000;
constexpr uint64_t DIST_TYPER = 0x004;
constexpr uint64_t DIST_IIDR = 0x008;
constexpr uint64_t DIST_ISENABLER0 = 0x100;
constexpr uint64_t DIST_ICENABLER0 = 0x180;
constexpr uint64_t DIST_ISPENDR0 = 0x200;
constexpr uint64_t DIST_ISACTIVER0 = 0x300;
constexpr uint64_t DIST_PRIORITY0 = 0x400;
constexpr uint64_t DIST_TARGET0 = 0x800;
constexpr uint64_t DIST_SGIR = 0xF00;
constexpr uint64_t DIST_CID0 = 0xF88;
constexpr uint64_t DIST_PID2 = 0xFE8;

/* CPU interface offsets. */
constexpr uint64_t CPU_CTL = 0x000;
constexpr uint64_t CPU_PMR = 0x004;
constexpr uint64_t CPU_IAR = 0x00C;
constexpr uint64_t CPU_EOIR = 0x010;

class Gicv2 : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_EQ(OEMU_OK, oemu_machine_init(&machine_, kRamBase, kRamSize, 4U));
    oemu_gicv2_init(&gic_, 64U);
    ASSERT_EQ(OEMU_OK,
              oemu_aspace_attach_device(&machine_.aspace, kDist, kRegion, &gic_.dist_ops));
    ASSERT_EQ(OEMU_OK,
              oemu_aspace_attach_device(&machine_.aspace, kCpu, kRegion, &gic_.cpu_ops));
  }
  void TearDown() override { oemu_machine_dispose(&machine_); }

  uint32_t rd(uint64_t off) {
    uint64_t v = 0xDEADBEEFULL;
    EXPECT_EQ(OEMU_OK,
              oemu_aspace_read(&machine_.aspace, kDist + off, OEMU_MEM_WORD, false, &v));
    return (uint32_t)v;
  }
  void wr(uint64_t off, uint32_t v) {
    EXPECT_EQ(OEMU_OK, oemu_aspace_write(&machine_.aspace, kDist + off, OEMU_MEM_WORD, v));
  }
  uint32_t cprd(uint64_t off) {
    uint64_t v = 0xDEADBEEFULL;
    EXPECT_EQ(OEMU_OK,
              oemu_aspace_read(&machine_.aspace, kCpu + off, OEMU_MEM_WORD, false, &v));
    return (uint32_t)v;
  }
  void cpwr(uint64_t off, uint32_t v) {
    EXPECT_EQ(OEMU_OK, oemu_aspace_write(&machine_.aspace, kCpu + off, OEMU_MEM_WORD, v));
  }

  oemu_machine machine_{};
  oemu_gicv2 gic_{};
};

// --- identity / reset -------------------------------------------------------

TEST_F(Gicv2, PeripheralIdMatchesGic400) {
  EXPECT_EQ(0x0DU, rd(DIST_CID0));
  EXPECT_EQ(0x2BU, rd(DIST_PID2)); /* the value irq-gic matches on */
  EXPECT_EQ(0x00U, rd(DIST_IIDR));
}

TEST_F(Gicv2, TyperReportsTwoGroupsOfSixtyFour) {
  // itlines (bits[4:0]) == groups-1 == 1; ncpu field (bits[23:16]) == 16.
  const uint32_t typer = rd(DIST_TYPER);
  EXPECT_EQ(1U, typer & 0x1FU);
  EXPECT_EQ(16U, (typer >> 16) & 0x1FU);
  EXPECT_EQ(64U, oemu_gicv2_nr_lines(&gic_));
}

TEST_F(Gicv2, StartsDisabled) {
  EXPECT_EQ(0U, rd(DIST_CTL));
  EXPECT_EQ(0U, cprd(CPU_CTL));
}

TEST_F(Gicv2, SpuriousAckWhenNothingPending) {
  wr(DIST_CTL, 1U);
  cpwr(CPU_CTL, 1U);
  cpwr(CPU_PMR, 0xFFU);
  EXPECT_EQ(OEMU_GICV2_SPURIOUS, cprd(CPU_IAR));
  EXPECT_EQ(0, oemu_gicv2_irq_level(&gic_));
}

// GICD_ITROUTER_n resets to 0b00000001 -- addressed to CPU0 -- and the guest is
// entitled to rely on that: Linux's gic_dist_init() brings up the distributor and
// leaves the affinity of an interrupt it has no reason to move. So a raised SPI
// has to be deliverable with *no* write to ITROUTER at all. Ours reset these to
// zero, measured as ITROUTER(33) == 0 with the console's line asserted: every
// device the guest never re-targeted was quietly un-plumbed, and the symptom was
// a received character that never reached its driver.
TEST_F(Gicv2, SpiTargetsThisCpuFromReset) {
  cpwr(CPU_PMR, 0xFFU);
  cpwr(CPU_CTL, 1U);
  wr(DIST_CTL, 1U);
  wr(DIST_ISENABLER0 + 4U, 0x2U);             /* enable line 33; routing left at reset */
  EXPECT_EQ(1U, rd(DIST_TARGET0 + 4U * 33U)); /* line 33's own router register */

  oemu_gicv2_set_pending(&gic_, 33U, true); /* the level source asserts */
  EXPECT_EQ(1, oemu_gicv2_irq_level(&gic_));
  EXPECT_EQ(33U, cprd(CPU_IAR));
  oemu_gicv2_set_pending(&gic_, 33U, false); /* ...and the level drops */
  EXPECT_EQ(0, oemu_gicv2_irq_level(&gic_));
}

// --- the acknowledge / EOI handshake ---------------------------------------

// Arm one SPI end-to-end the way the driver would: target, enable, pending
// (priority stays at the 0x80 reset value); then watch the CPU interface
// acknowledge it and the EOI retire it. Targets/priorities are one byte per
// line, so SPI 33 lives in word 8 (offset +0x20), byte 1.
TEST_F(Gicv2, SpiHandshakeRoundTrip) {
  wr(DIST_TARGET0 + 4U * 33U, 0x1U); /* line 33's own register -> cpu 0 */
  wr(DIST_ISENABLER0 + 4U, 0x2U);    /* enable word 1 -> line 33 */
  wr(DIST_ISPENDR0 + 4U, 0x2U);      /* pend  word 1 -> line 33 */
  cpwr(CPU_PMR, 0xFFU);
  cpwr(CPU_CTL, 1U);
  wr(DIST_CTL, 1U);

  EXPECT_EQ(1, oemu_gicv2_irq_level(&gic_));
  EXPECT_EQ(33U, cprd(CPU_IAR));
  EXPECT_EQ(0, oemu_gicv2_irq_level(&gic_)); /* consumed by the ack */
  EXPECT_EQ(1U, (rd(DIST_ISACTIVER0 + 4U) >> 1) & 1U);
  cpwr(CPU_EOIR, 33U);
  EXPECT_EQ(0U, (rd(DIST_ISACTIVER0 + 4U) >> 1) & 1U);
}

TEST_F(Gicv2, DisabledDistributorNeverInterrupts) {
  oemu_gicv2_set_pending(&gic_, 0U, true);
  wr(DIST_ISENABLER0, 1U); /* SGI 0 enabled, target fixed to this CPU */
  cpwr(CPU_CTL, 1U);
  cpwr(CPU_PMR, 0xFFU);
  wr(DIST_CTL, 0U); /* distributor off */
  EXPECT_EQ(0, oemu_gicv2_irq_level(&gic_));
  wr(DIST_CTL, 1U); /* distributor on -> now it fires */
  EXPECT_EQ(1, oemu_gicv2_irq_level(&gic_));
  EXPECT_EQ(0U, cprd(CPU_IAR));
}

TEST_F(Gicv2, PriorityMaskDropsLowPrecedenceLines) {
  oemu_gicv2_set_pending(&gic_, 1U, true);
  wr(DIST_ISENABLER0, 1U << 1);
  cpwr(CPU_CTL, 1U);
  cpwr(CPU_PMR, 0x00U); /* mask everything (priority reset value is 0x80) */
  wr(DIST_CTL, 1U);
  EXPECT_EQ(0, oemu_gicv2_irq_level(&gic_));
  cpwr(CPU_PMR, 0xFFU); /* unmask -> the 0x80 line now clears the mask */
  EXPECT_EQ(1, oemu_gicv2_irq_level(&gic_));
}

// Two pending lines: the one with the lower priority *value* wins the ack.
TEST_F(Gicv2, LowestPriorityValueAcknowledgedFirst) {
  wr(DIST_PRIORITY0 + 0x20U, (0x20U << 0) | (0xF0U << 8)); /* line 32=0x20, 33=0xF0 */
  wr(DIST_TARGET0 + 0x20U, (1U << 0) | (1U << 8));         /* both -> cpu 0 */
  wr(DIST_ISENABLER0 + 4U, (1U << 0) | (1U << 1));         /* word 1 -> lines 32,33 */
  wr(DIST_ISPENDR0 + 4U, (1U << 0) | (1U << 1));
  cpwr(CPU_PMR, 0xFFU);
  cpwr(CPU_CTL, 1U);
  wr(DIST_CTL, 1U);
  EXPECT_EQ(32U, cprd(CPU_IAR)); /* 0x20 beats 0xF0 */
  cpwr(CPU_EOIR, 32U);           /* retire it: 0xF0 can now beat the floor */
  EXPECT_EQ(33U, cprd(CPU_IAR)); /* then the other */
}

// --- banked state readback --------------------------------------------------

TEST_F(Gicv2, EnableBankReadsBack) {
  wr(DIST_ISENABLER0, 0xF0U);
  EXPECT_EQ(0xF0U, rd(DIST_ISENABLER0));
  wr(DIST_ICENABLER0, 0x0FU); /* clears the low nibble, leaves the top */
  EXPECT_EQ(0xF0U, rd(DIST_ISENABLER0));
}

TEST_F(Gicv2, SgiPpiTargetFixedSpiProgrammable) {
  // GICD_ITROUTERn is one register per interrupt, only the low byte implemented:
  // line 0's router reads back as 0x01, and a whole-word write is not four
  // targets. SGI/PPI (0-31) are fixed to the single CPU, so writes there are
  // ignored.
  EXPECT_EQ(0x00000001U, rd(DIST_TARGET0));
  wr(DIST_TARGET0, 0x000000FFU); /* ignored for line 0 */
  EXPECT_EQ(0x00000001U, rd(DIST_TARGET0));
  // The bytes above the target list are RES0: a write of 0xFF000000 must not
  // appear as an affinity, or a driver that writes a full word gets a ghost CPU.
  wr(DIST_TARGET0 + 4U * 34U, 0xFF000000U);
  EXPECT_EQ(0x00000000U, rd(DIST_TARGET0 + 4U * 34U));
  // SPI (32+) reset to 0b00000001 -- CPU0 -- and are writable (word 8 = lines
  // 32+). Reading zero here was pinning half the bug: the driver may leave a
  // reset value alone, and then a level interrupt from a device is raised into a
  // distributor that will not route it. See SpiTargetsThisCpuFromReset.
  EXPECT_EQ(0x00000001U, rd(DIST_TARGET0 + 4U * 33U)); /* SPI 1 == interrupt 33 */
  wr(DIST_TARGET0 + 4U * 33U, 0x00000001U);
  EXPECT_EQ(0x00000001U, rd(DIST_TARGET0 + 4U * 33U));
}

// --- RAZ/WI and no-fault contract ------------------------------------------

TEST_F(Gicv2, UnmodelledOffsetsRazWi) {
  uint64_t v = 0xDEADBEEFULL;
  // A gap between modelled blocks must read zero and never fault...
  EXPECT_EQ(OEMU_OK,
            oemu_aspace_read(&machine_.aspace, kDist + 0x040ULL, OEMU_MEM_WORD, false, &v));
  EXPECT_EQ(0U, v);
  // ...and swallow writes.
  EXPECT_EQ(OEMU_OK,
            oemu_aspace_write(&machine_.aspace, kDist + 0x040ULL, OEMU_MEM_WORD, 0x123U));
  // The SGIR is accepted (recorded) rather than faulting.
  EXPECT_EQ(OEMU_OK,
            oemu_aspace_write(&machine_.aspace, kDist + DIST_SGIR, OEMU_MEM_WORD, 0x0100U));
}

// The set_pending seam the wired sources (UART, timer) drive.
TEST_F(Gicv2, SetPendingSeamDrivesTheLine) {
  wr(DIST_ISENABLER0 + 4U, 1U); /* enable word 1 -> SPI line 32 */
  wr(DIST_TARGET0 + 0x20U, 1U); /* line 32 -> cpu0 */
  cpwr(CPU_CTL, 1U);
  cpwr(CPU_PMR, 0xFFU);
  wr(DIST_CTL, 1U);
  EXPECT_EQ(0, oemu_gicv2_irq_level(&gic_));
  oemu_gicv2_set_pending(&gic_, 32U, true);
  EXPECT_EQ(1, oemu_gicv2_irq_level(&gic_));
  oemu_gicv2_set_pending(&gic_, 32U, false);
  EXPECT_EQ(0, oemu_gicv2_irq_level(&gic_));
}

}  // namespace
