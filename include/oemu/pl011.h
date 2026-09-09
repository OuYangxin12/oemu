/*
 * ARM PL011 UART -- the console of every arm64 boot.
 *
 * A device is a struct, not an allocation: its rings and register file are
 * embedded, so attaching one costs the caller's storage and the bus
 * callbacks stay allocation-free by construction. The observable behaviour
 * is copied from qemu-system-aarch64's model, measured with
 * tests/guest/pl011_probe.S: unconditionally-pass-through TX (the byte
 * escapes even while CR disables the UART), a 16-deep RX ring, RIS/IMSC
 * interrupt state, and the peripheral ID table the PL011 driver reads.
 *
 * With the FIFO off (CR.FEN=0 -- the state the kernel's console driver
 * leaves it in) pass-through is immediate: the DR write hands the byte to
 * the sink before it returns and FR.TXFE is honest the moment the guest
 * re-reads it. Deferring those bytes to a pump turned out to lie about TX
 * while the ring held them, and Linux's earlycon spun on that lie until its
 * panic printout was cut mid-word. Only FIFO mode (CR.FEN=1) queues, into
 * a 64 KiB ring drained by oemu_pl011_pump (oldest dropped on overflow --
 * TX must never block the vCPU). RX arrives through
 * oemu_pl011_inject, which answers OEMU_ERR_FULL when the ring is full
 * and OEMU_ERR_STATE when the receiver is disabled: the caller owns the
 * drop policy, exactly as the host stdin pump will.
 */
#ifndef OEMU_PL011_H
#define OEMU_PL011_H

#include "oemu/device.h"
#include "oemu/macros.h"
#include "oemu/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

OEMU_BEGIN_DECLS

/* Ring depths: fixed, embedded. The RX ring matches the real FIFO. The TX
 * ring is deliberately deeper than the 32-entry hardware: the model drains
 * TX at run-loop slice boundaries (BOOT_QUANTUM = 1e6 instructions), not at a
 * baud rate, so a burst longer than the slice silently loses the oldest byte.
 * 64 bytes covered a banner; a booting Linux emits tens of kilobytes of
 * printk inside one slice, which is how the M5 gate's log came back truncated
 * with the interesting lines missing. 64 KiB holds a slice's burst; the drop
 * path stays as the last-resort overflow signal the gate refuses to ignore. */
#define OEMU_PL011_TX_RING 65536U
#define OEMU_PL011_RX_RING 16U

/* One output byte, handed to the installer's sink by oemu_pl011_pump. */
typedef void (*oemu_pl011_sink)(void *user, unsigned char byte);

typedef struct oemu_pl011 {
  oemu_device_ops ops; /* attach through oemu_aspace_attach_device */
  /* Register file, reset state per the QEMU probe (see docs task card). */
  uint32_t dr;    /* data, last read (read-only architecturally) */
  uint32_t fr;    /* flags */
  uint32_t cr;    /* control: reset 0x90 = TXE|LOOPBACK */
  uint32_t ibrd;  /* integer baud divisor */
  uint32_t fbrd;  /* fraction baud divisor */
  uint32_t lcr_h; /* line control, high */
  uint32_t fifls; /* fifo level select */
  uint32_t imsc;  /* interrupt mask */
  uint32_t ris;   /* raw interrupt status */
  /* TX ring: written bytes queue here until pump hands them out. */
  unsigned char tx[OEMU_PL011_TX_RING];
  unsigned tx_head;  /* next index pump reads */
  unsigned tx_count; /* bytes queued */
  /* RX ring: injected bytes queue here until the guest reads DR. */
  unsigned char rx[OEMU_PL011_RX_RING];
  unsigned rx_head;
  unsigned rx_tail;
  unsigned rx_count;
  /* Counters, so tests and the boot path can watch the device work. */
  uint64_t tx_emitted; /* bytes handed to the sink */
  uint64_t tx_dropped; /* bytes lost to a full TX ring (console loss, gate-visible) */
  uint64_t rx_dropped; /* loopback bytes the receiver had no room for (RX loss) */
  oemu_pl011_sink sink;
  void *sink_user;
} oemu_pl011;

/* Fill in a reset PL011 whose sink is `sink(user)`; NULL sink means the
 * pump still drains but discards. No allocation: safe for a stack or
 * static uart. */
void oemu_pl011_init(oemu_pl011 *uart, oemu_pl011_sink sink, void *sink_user);

/* Drain the TX ring into the sink. Called by whoever runs the machine
 * each round (the boot loop, or a test between steps). Returns the number
 * of bytes emitted. */
size_t oemu_pl011_pump(oemu_pl011 *uart);

/* Bytes lost to a full TX ring since init. A console sink that greps the log
 * for markers must ask this before concluding the guest never printed them:
 * TX never blocks the vCPU, so a byte that finds the ring full is dropped, and
 * the drop is counted rather than hidden. */
uint64_t oemu_pl011_tx_dropped(const oemu_pl011 *uart);

/*
 * Deliver one host-side byte to the RX ring. OEMU_ERR_STATE when the
 * receiver is disabled (UARTEN or RXE clear), OEMU_ERR_FULL when the ring
 * is full -- the caller keeps or drops the byte, the device never does.
 */
OEMU_NODISCARD oemu_status oemu_pl011_inject(oemu_pl011 *uart, unsigned char byte);

/* Level signal for the interrupt line: nonzero exactly when MIS != 0.
 * Nothing wires it until the interrupt controller exists (M4b). */
int oemu_pl011_irq_level(const oemu_pl011 *uart);

OEMU_END_DECLS

#endif /* OEMU_PL011_H */
