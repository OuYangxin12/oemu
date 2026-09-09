/*
 * ARM PL011 UART -- see include/oemu/pl011.h.
 *
 * The behaviour mirrored here is QEMU's, as measured by
 * tests/guest/pl011_probe.S on qemu-system-aarch64 10.2.1: TX passes
 * through whatever CR says (a DR write emits even with the UART disabled
 * and the loopback bit set); the peripheral IDs answer 11/10/14/00 and
 * the PrimeCell IDs all zero; FR starts at zero and only the event
 * handlers raise it. Deviating from the oracle is a bug even when the TRM
 * agrees with us -- the guests we must boot have already chosen a side.
 */
#include "oemu/check.h"

#include <stddef.h>
#include <string.h>

#include "pl011_internal.h"

/* --- bus contract --------------------------------------------------------- */

/* The device decodes a whole 4 KiB page: unimplemented offsets read as
 * their register file does and swallow writes, never a Data Abort -- a
 * probing driver must see a quiet register bank, not an exception. */

static oemu_status pl011_read(void *ctx, uint64_t offset, oemu_mem_size size,
                              uint64_t *value_out);
static oemu_status pl011_write(void *ctx, uint64_t offset, oemu_mem_size size, uint64_t value);

void oemu_pl011_init(oemu_pl011 *uart, oemu_pl011_sink sink, void *sink_user) {
  OEMU_REQUIRE(uart != NULL, "NULL oemu_pl011");
  (void)memset(uart, 0, sizeof(*uart));
  uart->cr = PL011_CR_RESET;
  uart->ops.ctx = uart;
  uart->ops.read = &pl011_read;
  uart->ops.write = &pl011_write;
  uart->sink = sink;
  uart->sink_user = sink_user;
  /* FR stays 0 like the oracle's reset: the flags register only wakes
   * once an event handler has run (oemu_pl011_internal_update_flags). */
}

void oemu_pl011_internal_update_flags(oemu_pl011 *uart) {
  /* An idle line is ready to transmit: TXFE (empty) set, TXFF (full) clear,
   * BUSY clear. This is what the console write path spins for (earlycon
   * waits on TXFE, the driver on TXFF/BUSY), so getting it wrong deadlocks the
   * first earlycon character. */
  uint32_t fr = PL011_FR_TXFE;
  if (uart->rx_count == 0U) {
    fr |= PL011_FR_RXFE;
  }
  /* The modem status lines are wired to nothing here, so they are read as low
   * -- measured on the oracle at an idle shell prompt, where FR is 0x90
   * (TXFE|RXFE) and not the 0x97 an asserted-CTS model returned. Nothing in the
   * guest's transmit path consults them while CTSEN stays clear, but inventing
   * them was still inventing. */
  /* TXFF is deliberately never set from the TX ring. The ring is host-side
   * batching: a sink consumes every byte as it is written, so the modelled
   * FIFO is never full -- exactly like the oracle, where the chardev drains the
   * transmit FIFO immediately and FR reads back TXFE|RXFE with TXFF clear.
   * Reporting TXFF from an un-pumped queue made the driver's tx_room() return 0,
   * so n_tty_write() slept on tty->write_wait and nothing ever woke it: the
   * drain was a host-side event with no interrupt to go with it. Measured: /init
   * printed its first line, its second write never returned, and the guest sat
   * idle at WFI (ESR_EL1=0x56000000) for the rest of the run. TX must never
   * block the vCPU; oemu_pl011_pump only empties a ring a sink-less caller left
   * filled. */
  uart->fr = fr;
}

static void pl011_tx_push(oemu_pl011 *uart, unsigned char byte) {
  if (uart->tx_count >= OEMU_PL011_TX_RING) {
    /* The ring is full because nobody pumped. Drop the oldest: TX must
     * not block the vCPU, and the lost byte is counted, not hidden. */
    uart->tx_head = (uart->tx_head + 1U) % OEMU_PL011_TX_RING;
    uart->tx_count--;
    uart->tx_dropped++;
  }
  const unsigned tail = (uart->tx_head + uart->tx_count) % OEMU_PL011_TX_RING;
  uart->tx[tail] = byte;
  uart->tx_count++;
}

static oemu_status pl011_read(void *ctx, uint64_t offset, oemu_mem_size size,
                              uint64_t *value_out) {
  oemu_pl011 *uart = (oemu_pl011 *)ctx;
  OEMU_REQUIRE((uart != NULL) && (value_out != NULL), "NULL pl011 read argument");
  if (size > OEMU_MEM_DWORD) {
    return OEMU_ERR_INVALID_ARG;
  }
  uint32_t v = 0U;
  switch (offset) {
    case PL011_REG_DR:
      /* Pop the RX ring; empty reads back zero. Draining the last byte
       * retires the receive status bit, per the oracle's read path. */
      if (uart->rx_count != 0U) {
        v = uart->rx[uart->rx_head];
        uart->rx_head = (uart->rx_head + 1U) % OEMU_PL011_RX_RING;
        uart->rx_count--;
        if (uart->rx_count == 0U) {
          uart->ris &= ~PL011_INT_RX;
        }
      }
      oemu_pl011_internal_update_flags(uart);
      break;
    case PL011_REG_FR:
      v = uart->fr;
      break;
    case PL011_REG_ILPR:
      v = 0U;
      break;
    case PL011_REG_IBRD:
      v = uart->ibrd;
      break;
    case PL011_REG_FBRD:
      v = uart->fbrd;
      break;
    case PL011_REG_LCR_H:
      v = uart->lcr_h;
      break;
    case PL011_REG_CR:
      v = uart->cr;
      break;
    case PL011_REG_FIFLS:
      v = uart->fifls;
      break;
    case PL011_REG_RSR:
      v = 0U; /* no framing/parity/overrun errors modelled */
      break;
    case PL011_REG_INTIM:
      v = uart->imsc;
      break;
    case PL011_REG_INTCLR:
      v = 0U;
      break;
    case PL011_REG_RIS:
      v = uart->ris;
      break;
    case PL011_REG_MIS:
      v = uart->ris & uart->imsc;
      break;
    case 0xFD0U:
    case 0xFD4U:
    case 0xFD8U:
    case 0xFDCU:
      v = 0U; /* PrimeCell IDs, measured all-zero on the oracle */
      break;
    case 0xFE0U:
      v = PL011_PID0;
      break;
    case 0xFE4U:
      v = PL011_PID1;
      break;
    case 0xFE8U:
      v = PL011_PID2;
      break;
    case 0xFECU:
      v = PL011_PID3;
      break;
    case 0xFF0U:
      v = PL011_CID0;
      break;
    case 0xFF4U:
      v = PL011_CID1;
      break;
    case 0xFF8U:
      v = PL011_CID2;
      break;
    case 0xFFCU:
      v = PL011_CID3;
      break;
    default:
      v = 0U; /* unimplemented: quiet zero, never a fault */
      break;
  }
  *value_out = v;
  return OEMU_OK;
}

static oemu_status pl011_write(void *ctx, uint64_t offset, oemu_mem_size size, uint64_t value) {
  oemu_pl011 *uart = (oemu_pl011 *)ctx;
  OEMU_REQUIRE(uart != NULL, "NULL pl011 write argument");
  if (size > OEMU_MEM_DWORD) {
    return OEMU_ERR_INVALID_ARG;
  }
  const uint32_t v = (uint32_t)value;
  switch (offset) {
    case PL011_REG_DR:
      /* Emitted unconditionally: measured (probe DROP1|A) that the oracle
       * passes a DR byte through even while CR disables the UART. The
       * loopback bit mirrors it into RX as well.
       *
       * With the FIFO off (CR.FEN=0 -- the state every real driver leaves
       * it in, including the kernel's arm,pl011 console driver) the oracle's
       * pass-through is IMMEDIATE: QEMU hands the byte to the chardev in the
       * write itself, so FR.TXFE is already reasserted when the guest's
       * transmit spin re-reads FR. A queue-then-pump design looked equivalent
       * from the outside but is not: earlycon spins on TXFE inside one
       * execution slice, and a deferred queue left FR lying -- Linux's
       * __pi_putc ate its own tail and the panic printout died mid-word.
       * The ring only models the FIFO (FEN=1), where bytes really do queue. */
      if ((uart->cr & PL011_CR_FEN) == 0U) {
        if (uart->sink != NULL) {
          uart->sink(uart->sink_user, (unsigned char)v);
        }
        uart->tx_emitted++;
      } else {
        pl011_tx_push(uart, (unsigned char)v);
      }
      uart->ris |= PL011_INT_TXIS;
      if (((uart->cr & PL011_CR_LBE) != 0U) && ((uart->cr & PL011_CR_RXE) != 0U)) {
        /* Loopback mirrors the byte into the receiver, and only when there is a
         * receiver: RXE is the same gate the host-injection path honours. The
         * reset CR is TXE|LBE with RXE clear, so charging these to tx_dropped
         * turned every earlycon byte into a phantom "console lost data" and the
         * boot gate refused a log that was complete. A full RX ring is an RX
         * loss and is counted as one. */
        if (oemu_pl011_inject(uart, (unsigned char)v) != OEMU_OK) {
          uart->rx_dropped++;
        }
      }
      break;
    case PL011_REG_RSR:
      break; /* ECR write: we raise no error status, so nothing to clear */
    case PL011_REG_ILPR:
      break; /* purge request: nothing of ours is latched */
    case PL011_REG_IBRD:
      uart->ibrd = v;
      break;
    case PL011_REG_FBRD:
      uart->fbrd = v;
      break;
    case PL011_REG_LCR_H:
      uart->lcr_h = v;
      break;
    case PL011_REG_CR:
      uart->cr = v;
      break;
    case PL011_REG_FIFLS:
      uart->fifls = v;
      break;
    case PL011_REG_INTIM:
      uart->imsc = v; /* the whole mask written at once (QEMU's read/write path) */
      break;
    case PL011_REG_INTCLR:
      uart->ris &= ~v;
      break;
    case PL011_REG_RIS:
    case PL011_REG_MIS:
      break; /* status registers: writes ignored, as on the oracle */
    default:
      break; /* unimplemented: swallowed */
  }
  oemu_pl011_internal_update_flags(uart);
  return OEMU_OK;
}

/* --- public service -------------------------------------------------------- */

uint64_t oemu_pl011_tx_dropped(const oemu_pl011 *uart) {
  OEMU_REQUIRE(uart != NULL, "NULL oemu_pl011 in oemu_pl011_tx_dropped");
  return uart->tx_dropped;
}

size_t oemu_pl011_pump(oemu_pl011 *uart) {
  OEMU_REQUIRE(uart != NULL, "NULL oemu_pl011");
  size_t emitted = 0U;
  while (uart->tx_count != 0U) {
    const unsigned char byte = uart->tx[uart->tx_head];
    uart->tx_head = (uart->tx_head + 1U) % OEMU_PL011_TX_RING;
    uart->tx_count--;
    if (uart->sink != NULL) {
      uart->sink(uart->sink_user, byte);
    }
    uart->tx_emitted++;
    emitted++;
  }
  oemu_pl011_internal_update_flags(uart);
  return emitted;
}

oemu_status oemu_pl011_inject(oemu_pl011 *uart, unsigned char byte) {
  if (uart == NULL) {
    return OEMU_ERR_INVALID_ARG;
  }
  if (((uart->cr & (PL011_CR_UARTEN | PL011_CR_RXE)) != (PL011_CR_UARTEN | PL011_CR_RXE))) {
    return OEMU_ERR_STATE; /* receiver off: the caller keeps the byte */
  }
  if (uart->rx_count >= OEMU_PL011_RX_RING) {
    return OEMU_ERR_FULL;
  }
  uart->rx[uart->rx_tail] = byte;
  uart->rx_tail = (uart->rx_tail + 1U) % OEMU_PL011_RX_RING;
  uart->rx_count++;
  uart->ris |= PL011_INT_RX;
  oemu_pl011_internal_update_flags(uart);
  return OEMU_OK;
}

int oemu_pl011_irq_level(const oemu_pl011 *uart) {
  OEMU_REQUIRE(uart != NULL, "NULL oemu_pl011");
  return (uart->ris & uart->imsc) != 0U;
}
