/*
 * Black-box tests for the PL011 model (include/oemu/pl011.h).
 *
 * Every case drives the device the way a guest driver does -- through the
 * bus: attach the model to a machine's address space, then read and write it
 * at the offsets in pl011_internal.h. Nothing pokes the struct directly, so
 * the bus contract (quiet unimplemented offsets, unconditional TX, gated RX)
 * is what is under test. The expected values are the ones measured off
 * qemu-system-aarch64 with tests/guest/pl011_probe.S: the peripheral ID
 * table, a FR that starts at zero, and a loopback that mirrors DR into RX.
 *
 * The model renumbers a few registers from the ARM TRM (FR at 0x04, CR at
 * 0x18, RIS/MIS at 0x30/0x34) to match the driver that boots the guests we
 * must run; the tests cite the same internal constants the driver does.
 */
#include "oemu/aspace.h"
#include "oemu/machine.h"
#include "oemu/status.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include <gtest/gtest.h>

#include "dev/pl011_internal.h"
#include "oemu/pl011.h"

namespace {

constexpr uint64_t kRamBase = 0x40000000ULL;
constexpr uint64_t kRamSize = 0x00010000ULL; /* 64 KiB: plenty, we never touch RAM */
constexpr uint64_t kUart = 0x09000000ULL;
constexpr uint64_t kUartSize = 0x1000ULL;

/* A sink that appends every emitted byte, so TX order is observable. */
std::vector<unsigned char> g_sink;
void collect(void *, unsigned char byte) {
  g_sink.push_back(byte);
}

class Pl011 : public ::testing::Test {
 protected:
  void SetUp() override {
    g_sink.clear();
    ASSERT_EQ(OEMU_OK, oemu_machine_init(&machine_, kRamBase, kRamSize, 4U));
    oemu_pl011_init(&uart_, &collect, nullptr);
    ASSERT_EQ(OEMU_OK,
              oemu_aspace_attach_device(&machine_.aspace, kUart, kUartSize, &uart_.ops));
  }
  void TearDown() override { oemu_machine_dispose(&machine_); }

  uint32_t rd(uint64_t off) {
    uint64_t v = 0xDEADBEEFULL;
    EXPECT_EQ(OEMU_OK,
              oemu_aspace_read(&machine_.aspace, kUart + off, OEMU_MEM_WORD, false, &v));
    return (uint32_t)v;
  }
  void wr(uint64_t off, uint32_t v) {
    EXPECT_EQ(OEMU_OK, oemu_aspace_write(&machine_.aspace, kUart + off, OEMU_MEM_WORD, v));
  }
  /* Turn the UART fully on and break loopback, the honest driver's opening
   * move; several behaviours are only visible with a clean CR. */
  void enable_no_loopback(void) {
    wr(PL011_REG_CR, PL011_CR_UARTEN | PL011_CR_TXE | PL011_CR_RXE);
  }

  oemu_machine machine_{};
  oemu_pl011 uart_{};
};

// --- reset state ------------------------------------------------------------

TEST_F(Pl011, ControlRegisterResetsToTxeAndLoopback) {
  EXPECT_EQ(PL011_CR_RESET, rd(PL011_REG_CR));
}

TEST_F(Pl011, FlagsRegisterIsQuietAtReset) {
  /* FR starts at zero on the oracle -- the flags only wake once an event
   * handler has recomputed them, which a fresh attach has not triggered. */
  EXPECT_EQ(0U, rd(PL011_REG_FR));
}

TEST_F(Pl011, ReadingEmptyDataRegisterYieldsZeroAndDoesNotTrap) {
  EXPECT_EQ(0U, rd(PL011_REG_DR));
}

TEST_F(Pl011, PeripheralIdsMatchTheOracle) {
  EXPECT_EQ(PL011_PID0, rd(0xFE0U));
  EXPECT_EQ(PL011_PID1, rd(0xFE4U));
  EXPECT_EQ(PL011_PID2, rd(0xFE8U));
  EXPECT_EQ(PL011_PID3, rd(0xFECU));
}

TEST_F(Pl011, PrimeCellIdsReadBackZero) {
  for (const uint64_t off : {0xFD0U, 0xFD4U, 0xFD8U, 0xFDCU}) {
    EXPECT_EQ(0U, rd(off)) << "offset " << off;
  }
}

TEST_F(Pl011, UnimplementedOffsetReadsZeroAndSwallowsWrites) {
  wr(0x200U, 0xFFFFFFFFU); /* a probe poking at unmapped space */
  EXPECT_EQ(0U, rd(0x200U));
  /* A faulting probe would have aborted the write path; the bus stayed calm. */
}

// --- TX: unconditional pass-through, ring drain, overflow --------------------

TEST_F(Pl011, TxByteIsEmittedEvenWithTheUartDisabled) {
  /* Measured (probe DROP1|A): a DR write escapes to the sink even while CR
   * says the UART is off, and with no loopback there is no RX mirror. */
  wr(PL011_REG_CR, 0U); /* clear TXE|LBE: transmitter nominally off */
  wr(PL011_REG_DR, 'A');
  EXPECT_EQ(1U, oemu_pl011_pump(&uart_));
  ASSERT_EQ(1U, g_sink.size());
  EXPECT_EQ((unsigned char)'A', g_sink[0]);
  EXPECT_EQ(1ULL, uart_.tx_emitted);
  EXPECT_EQ(0ULL, uart_.tx_dropped);
}

TEST_F(Pl011, TxRingDrainsInByteOrder) {
  enable_no_loopback();
  wr(PL011_REG_DR, 'o');
  wr(PL011_REG_DR, 'k');
  wr(PL011_REG_DR, '!');
  EXPECT_EQ(3U, oemu_pl011_pump(&uart_));
  EXPECT_EQ(std::vector<unsigned char>({'o', 'k', '!'}), g_sink);
  EXPECT_EQ(0U, uart_.tx_count);
  EXPECT_EQ(0U, oemu_pl011_pump(&uart_)); /* drained: nothing left to emit */
}

TEST_F(Pl011, TxRingDropsOldestWhenOverflowed) {
  /* Nobody pumped, so the ring fills; the model must keep the newest bytes
   * and count, not hide, the loss -- TX is never allowed to block the vCPU. */
  enable_no_loopback();
  for (unsigned i = 0U; i < 70U; i++) {
    wr(PL011_REG_DR, 'A' + (i % 26U));
  }
  EXPECT_EQ(OEMU_PL011_TX_RING, uart_.tx_count); /* 64 kept */
  EXPECT_EQ(6ULL, uart_.tx_dropped);             /* 70 - 64 lost */
}

TEST_F(Pl011, FlagsShowBusyWhileTxQueuedThenEmptyAfterPump) {
  enable_no_loopback();
  wr(PL011_REG_DR, 'x');
  const uint32_t busy = rd(PL011_REG_FR);
  EXPECT_NE(0U, busy & PL011_FR_BUSY);
  EXPECT_EQ(0U, busy & PL011_FR_TXFE); /* queue not drained: TX not empty */
  (void)oemu_pl011_pump(&uart_);
  const uint32_t idle = rd(PL011_REG_FR);
  EXPECT_NE(0U, idle & PL011_FR_TXFE); /* drained: TXFE reasserted */
  EXPECT_EQ(0U, idle & PL011_FR_BUSY);
}

// --- RIS / interrupt state --------------------------------------------------

TEST_F(Pl011, TransmitIntStatusSetsOnDataWrite) {
  enable_no_loopback();
  wr(PL011_REG_DR, 'z');
  EXPECT_NE(0U, rd(PL011_REG_RIS) & PL011_INT_TIEM);
}

TEST_F(Pl011, IntStatusWriteIsIgnored) {
  enable_no_loopback();
  wr(PL011_REG_DR, 'z');
  const uint32_t before = rd(PL011_REG_RIS);
  wr(PL011_REG_RIS, 0xFFFFFFFFU); /* RIS is read-only on the oracle */
  EXPECT_EQ(before, rd(PL011_REG_RIS));
}

TEST_F(Pl011, IntClearRetiresTheSelectedBits) {
  enable_no_loopback();
  wr(PL011_REG_DR, 'z');
  EXPECT_NE(0U, rd(PL011_REG_RIS) & PL011_INT_TIEM);
  wr(PL011_REG_INTCLR, PL011_INT_TIEM);
  EXPECT_EQ(0U, rd(PL011_REG_RIS) & PL011_INT_TIEM);
}

TEST_F(Pl011, IrqLevelFollowsMaskedStatus) {
  enable_no_loopback();
  wr(PL011_REG_INTIM, PL011_INT_TIEM);        /* unmask transmit */
  EXPECT_EQ(0, oemu_pl011_irq_level(&uart_)); /* no event yet */
  wr(PL011_REG_DR, 'z');                      /* raises RIS.TIEM */
  EXPECT_EQ(1, oemu_pl011_irq_level(&uart_)); /* masked and raised: line high */
  EXPECT_NE(0U, rd(PL011_REG_MIS) & PL011_INT_TIEM);
  wr(PL011_REG_INTCLR, PL011_INT_TIEM);
  EXPECT_EQ(0, oemu_pl011_irq_level(&uart_)); /* cleared: line low */
}

TEST_F(Pl011, MaskedEventLeavesTheLineLow) {
  enable_no_loopback();
  /* imsc still zero (reset): an unmasked-by-omission event raises RIS but
   * never the line. */
  wr(PL011_REG_DR, 'z');
  EXPECT_NE(0U, rd(PL011_REG_RIS) & PL011_INT_TIEM);
  EXPECT_EQ(0, oemu_pl011_irq_level(&uart_));
  EXPECT_EQ(0U, rd(PL011_REG_MIS) & PL011_INT_TIEM);
}

// --- RX: injection is gated, ringed, and clears on drain --------------------

TEST_F(Pl011, InjectIsRefusedWhileTheReceiverIsOff) {
  /* At reset the receiver is not enabled (no UARTEN|RXE): the device says
   * "not now" and the caller keeps the byte -- it never drops it itself. */
  EXPECT_EQ(OEMU_ERR_STATE, oemu_pl011_inject(&uart_, 'x'));
}

TEST_F(Pl011, InjectThenReadBackAndRetire) {
  enable_no_loopback(); /* UARTEN|TXE|RXE: the receiver is live */
  ASSERT_EQ(OEMU_OK, oemu_pl011_inject(&uart_, 'Q'));
  EXPECT_EQ(1U, uart_.rx_count);
  EXPECT_NE(0U, rd(PL011_REG_RIS) & PL011_INT_RLIS);
  EXPECT_EQ(0U, rd(PL011_REG_FR) & PL011_FR_RXFE); /* a byte is waiting */
  EXPECT_EQ((uint32_t)'Q', rd(PL011_REG_DR));      /* DR read pops it */
  EXPECT_EQ(0U, uart_.rx_count);
  EXPECT_EQ(0U, rd(PL011_REG_RIS) & PL011_INT_RLIS); /* last byte read: bit retires */
  EXPECT_NE(0U, rd(PL011_REG_FR) & PL011_FR_RXFE);   /* empty again */
}

TEST_F(Pl011, InjectFillsTheRingThenRefuses) {
  enable_no_loopback();
  for (unsigned i = 0U; i < OEMU_PL011_RX_RING; i++) {
    ASSERT_EQ(OEMU_OK, oemu_pl011_inject(&uart_, (unsigned char)(i + 1U)));
  }
  EXPECT_EQ(OEMU_ERR_FULL, oemu_pl011_inject(&uart_, 'Z'));
  EXPECT_EQ(OEMU_PL011_RX_RING, uart_.rx_count);
}

TEST_F(Pl011, InjectWithNullDeviceIsAnArgumentError) {
  EXPECT_EQ(OEMU_ERR_INVALID_ARG, oemu_pl011_inject(nullptr, 'x'));
}

// --- loopback ---------------------------------------------------------------

TEST_F(Pl011, LoopbackMirrorsTxIntoRx) {
  /* With the receiver live and loopback set, a DR write lands in RX as well
   * as the TX ring: the classic self-test the driver runs at open. */
  wr(PL011_REG_CR, PL011_CR_UARTEN | PL011_CR_RXE | PL011_CR_LBE);
  wr(PL011_REG_DR, 'S');
  EXPECT_EQ(1U, uart_.rx_count);
  (void)oemu_pl011_pump(&uart_); /* drain the TX copy, keep the RX copy */
  EXPECT_EQ((uint32_t)'S', rd(PL011_REG_DR));
}

// --- configuration registers round-trip -------------------------------------

TEST_F(Pl011, BaudAndLineRegistersRoundTrip) {
  wr(PL011_REG_IBRD, 0x0018U);
  wr(PL011_REG_FBRD, 0x33U);
  wr(PL011_REG_LCR_H, 0x70U);
  wr(PL011_REG_FIFLS, 0x02U);
  EXPECT_EQ(0x0018U, rd(PL011_REG_IBRD));
  EXPECT_EQ(0x33U, rd(PL011_REG_FBRD));
  EXPECT_EQ(0x70U, rd(PL011_REG_LCR_H));
  EXPECT_EQ(0x02U, rd(PL011_REG_FIFLS));
}

TEST_F(Pl011, IntMaskSetAndClearAdjustImMask) {
  wr(PL011_REG_INTMASKSET, PL011_INT_RLIS);
  EXPECT_NE(0U, rd(PL011_REG_INTIM) & PL011_INT_RLIS);
  wr(PL011_REG_INTMASKCLR, PL011_INT_RLIS);
  EXPECT_EQ(0U, rd(PL011_REG_INTIM) & PL011_INT_RLIS);
}

TEST_F(Pl011, SinklessPumpStillDrainsAndCounts) {
  /* A NULL sink means the caller does not want the bytes, but the ring must
   * still empty and the emit counter still advance -- TX never blocks. */
  oemu_pl011 bare;
  oemu_pl011_init(&bare, nullptr, nullptr);
  /* loopback is set at reset with no receiver, so the RX mirror is refused
   * and counted as a drop; the TX byte itself still queues. */
  ASSERT_EQ(OEMU_OK,
            oemu_aspace_attach_device(&machine_.aspace, 0x0A000000ULL, kUartSize, &bare.ops));
  ASSERT_EQ(OEMU_OK, oemu_aspace_write(&machine_.aspace, 0x0A000000ULL + PL011_REG_CR,
                                       OEMU_MEM_WORD, 0U));
  ASSERT_EQ(OEMU_OK, oemu_aspace_write(&machine_.aspace, 0x0A000000ULL + PL011_REG_DR,
                                       OEMU_MEM_WORD, 'z'));
  EXPECT_EQ(1U, oemu_pl011_pump(&bare));
  EXPECT_EQ(1ULL, bare.tx_emitted);
  EXPECT_TRUE(g_sink.empty());
}

}  // namespace
