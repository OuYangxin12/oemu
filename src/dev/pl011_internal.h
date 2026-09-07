/*
 * Internal surface of the PL011 model: register offsets and bit names,
 * numbered exactly as the driver numbers them, so both implementation and
 * tests speak offsets instead of arithmetic on 0x00.
 */
#ifndef OEMU_SRC_DEV_PL011_INTERNAL_H
#define OEMU_SRC_DEV_PL011_INTERNAL_H

#include "oemu/macros.h"

#include "oemu/pl011.h"

OEMU_BEGIN_DECLS

/* Offsets (drivers/amba/serial.h numbering, matching the TRM). */
#define PL011_REG_DR         0x00U
#define PL011_REG_FR         0x04U
#define PL011_REG_ILPR       0x08U
#define PL011_REG_IBRD       0x0CU
#define PL011_REG_FBRD       0x10U
#define PL011_REG_LCR_H      0x14U
#define PL011_REG_CR         0x18U
#define PL011_REG_FIFLS      0x1CU
#define PL011_REG_INTIM      0x20U
#define PL011_REG_INTCLR     0x24U
#define PL011_REG_INTMASKSET 0x28U
#define PL011_REG_INTMASKCLR 0x2CU
#define PL011_REG_RIS        0x30U
#define PL011_REG_MIS        0x34U
#define PL011_REG_DMACR1     0x38U
#define PL011_REG_DMACR2     0x3CU

/* FR bits. */
#define PL011_FR_CTS  0x01U
#define PL011_FR_DSR  0x02U
#define PL011_FR_DCD  0x04U
#define PL011_FR_BUSY 0x08U
#define PL011_FR_RXFE 0x10U
#define PL011_FR_TXFE 0x20U
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
 * event): RXFE/DSR/DCD/CTS ride on an empty RX ring, TXFE always set --
 * our TX never stalls the vCPU. */
void oemu_pl011_internal_update_flags(oemu_pl011 *uart);

OEMU_END_DECLS

#endif /* OEMU_SRC_DEV_PL011_INTERNAL_H */
