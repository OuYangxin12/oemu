/*
 * The system-register table, exposed for white-box tests.
 *
 * Not part of the public API: production code outside src/sysreg/ must use the
 * encoding-based accessors in oemu/sysreg.h. Tests reach this header to
 * validate the table wholesale -- every row well-formed, encodings matching
 * the assembler-harvested constants, reset values actually applied -- which a
 * pure read/write API could only cover one encoding at a time.
 */
#ifndef OEMU_SRC_SYSREG_INTERNAL_H
#define OEMU_SRC_SYSREG_INTERNAL_H

#include "oemu/sysreg.h"

#include <stddef.h>
#include <stdint.h>

OEMU_BEGIN_DECLS

/*
 * Row behaviour beyond plain read/write storage. RO and WI differ in what a
 * write means: RO writes trap (the register architecturally cannot be
 * written), WI writes succeed and change nothing (RAZ/WI stubs such as
 * CSSELR, where honouring the write would imply modelling cache selection).
 */
typedef enum oemu_sysreg_flags {
  OEMU_SYSREG_F_NONE = 0,
  OEMU_SYSREG_F_RO = 1U << 0, /* read-only: writes return OEMU_ERR_UNSUPPORTED */
  OEMU_SYSREG_F_WI = 1U << 1  /* RAZ/WI: reads return 0, writes are discarded */
} oemu_sysreg_flags;

/* One system register. A row either stores its value at `offset` inside
 * oemu_sysregs (plain fields; write_mask selects the writable bits) or hands
 * reads and writes to `get`/`set` (registers whose backing lives elsewhere:
 * SP_EL0 and NZCV in oemu_regs, SPSel/DAIF packed inside pstate, CurrentEL
 * computed). WI rows have neither storage nor callbacks. */
typedef struct oemu_sysreg_row {
  const char *name;
  uint32_t sel;        /* 14-bit encoding, direction bit already stripped */
  uint8_t min_el;      /* lowest oemu_el that may access the register */
  uint8_t flags;       /* oemu_sysreg_flags */
  uint16_t offset;     /* offsetof(oemu_sysregs, field); 0 for callback rows */
  uint64_t write_mask; /* bits a write may change; offset rows only */
  uint64_t reset_value;
  uint64_t (*get)(const oemu_sysregs *sr);
  void (*set)(oemu_sysregs *sr, uint64_t value);
} oemu_sysreg_row;

typedef struct oemu_sysreg_table {
  const oemu_sysreg_row *rows;
  size_t count;
} oemu_sysreg_table;

/* The whole table, ascending by `sel`. */
oemu_sysreg_table oemu_sysreg_internal_table(void);

/* First row whose `sel` matches, or NULL. Linear scan: the table is small and
 * the step path must stay free of setup work. */
const oemu_sysreg_row *oemu_sysreg_internal_find(uint32_t sel);

OEMU_END_DECLS

#endif /* OEMU_SRC_SYSREG_INTERNAL_H */
