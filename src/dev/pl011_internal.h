/*
 * Internal surface of the PL011 model: register offsets and bit names,
 * numbered exactly as the driver numbers them, so both implementation and
 * tests speak offsets instead of arithmetic on 0x00.
 *
 * The numbering here is the ARM PL011 TRM's, which is also what the kernel's
 * arm,pl011 driver (include/linux/amba/serial.h) and QEMU implement: FR at
 * 0x18, CR at 0x30, IMSC at 0x38, RIS/MIS at 0x3C/0x40, ICR at 0x44. An
 * earlier draft of this header renumbered them (FR at 0x04, CR at 0x18) which
 * silently mis-served a real guest driver's reads and writes -- a guest that
 * reads transmit-empty at 0x18 must get the flag register, not the control
 * one. The offsets below are the fix, cross-checked against the driver header.
 */
#ifndef OEMU_SRC_DEV_PL011_INTERNAL_H
#define OEMU_SRC_DEV_PL011_INTERNAL_H

#include "oemu/macros.h"

#include "oemu/pl011.h"

OEMU_BEGIN_DECLS

/* Offsets (drivers/tty/serial/amba-pl011.c REG_* numbering, matching the TRM).
 * The PL011 register file has no separate interrupt mask set/clear registers:
 * the driver writes the whole mask to IMSC in one go, so there is no SET/CLR
 * pair to model. */
#define PL011_REG_DR     0x00U
#define PL011_REG_RSR    0x04U /* receive status (read); error clear ECR (write) */
#define PL011_REG_FR     0x18U /* flag register (read only) */
#define PL011_REG_ILPR   0x20U
#define PL011_REG_IBRD   0x24U
#define PL011_REG_FBRD   0x28U
#define PL011_REG_LCR_H  0x2CU
#define PL011_REG_CR     0x30U
#define PL011_REG_FIFLS  0x34U /* interrupt fifo level select */
#define PL011_REG_INTIM  0x38U /* interrupt mask (IMSC): read or write whole */
#define PL011_REG_RIS    0x3CU /* raw interrupt status */
#define PL011_REG_MIS    0x40U /* masked interrupt status */
#define PL011_REG_INTCLR 0x44U /* interrupt clear (ICR) */
#define PL011_REG_DMACR1 0x48U
#define PL011_REG_DMACR2 0x4CU

/* FR bits (UART011_FR_* in the driver header). Two transmit flags, not one:
 * TXFE (empty) asserts when the transmitter has drained, TXFF (full) when it
 * cannot take another byte. The early and normal console write paths spin on
 * TXFF going clear then, for earlycon, on TXFE going set -- so an idle line
 * must present TXFE set and TXFF clear or the console hangs. */
#define PL011_FR_CTS  0x001U
#define PL011_FR_DSR  0x002U
#define PL011_FR_DCD  0x004U
#define PL011_FR_BUSY 0x008U
#define PL011_FR_RXFE 0x010U
#define PL011_FR_TXFF 0x020U /* transmit fifo full: the driver's "wait" bit */
#define PL011_FR_RXFF 0x040U
#define PL011_FR_TXFE 0x080U /* transmit fifo empty: the idle/ready flag */
#define PL011_FR_RI   0x100U

/* CR bits. */
#define PL011_CR_UARTEN 0x01U
#define PL011_CR_SIREN  0x02U
#define PL011_CR_SIRLP  0x04U
#define PL011_CR_LBE    0x80U
#define PL011_CR_TXE    0x100U
#define PL011_CR_RXE    0x200U
/* Measured QEMU reset value: TXE | LBE. */
#define PL011_CR_RESET (PL011_CR_TXE | PL011_CR_LBE)

/* RIS/MIS/IMSC/ICR bits. */
#define PL011_INT_RLIS 0x01U /* receive */
#define PL011_INT_TIEM 0x20U /* transmit */
#define PL011_INT_RTEM 0x40U /* receive timeout */
#define PL011_INT_OEIS 0x08U /* overflow */
#define PL011_INT_BEIS 0x02U /* break */
#define PL011_INT_PEIS 0x04U /* parity */
#define PL011_INT_FEIS 0x80U /* framing */

/* Peripheral ID at 0xFE0..0xFEC, measured off the QEMU oracle. */
#define PL011_PID0 0x11U
#define PL011_PID1 0x10U
#define PL011_PID2 0x14U
#define PL011_PID3 0x00U

/* Recompute the flag register from the rings (QEMU does the same on every
 * event): an idle line presents TXFE (empty, ready) with RXFE and the modem
 * lines, our TX ring never blocks the vCPU; a backed-up ring presents TXFF and
 * BUSY with TXFE withdrawn so a guest's transmit spin actually sees it. */
void oemu_pl011_internal_update_flags(oemu_pl011 *uart);

OEMU_END_DECLS

#endif /* OEMU_SRC_DEV_PL011_INTERNAL_H */
