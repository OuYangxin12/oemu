/*
 * Internal interface of the regs module -- NOT part of the public API.
 *
 * Everything here is a pure function: no oemu_regs instance, no allocation, no
 * hidden state. That is deliberate. The tricky parts of the register model are
 * width truncation, the 16-entry condition table and the carry/overflow
 * derivation, and all three are exhaustively testable at this level, where a
 * test can enumerate every input instead of constructing register state.
 *
 * Rules for this pattern:
 *   - never installed, never included by another module's public header;
 *   - symbols carry the oemu_regs_internal_ prefix to signal their status;
 *   - a change here is not a public API break.
 */
#ifndef OEMU_SRC_REGS_INTERNAL_H
#define OEMU_SRC_REGS_INTERNAL_H

#include "oemu/macros.h"
#include "oemu/regs.h"

#include <stdbool.h>
#include <stdint.h>

OEMU_BEGIN_DECLS

/*
 * Narrows `value` to the operand width: a no-op at 64 bits, the low 32 bits
 * otherwise. Used on both read and write paths, which is what makes a W write
 * zero-extend rather than merge.
 */
uint64_t oemu_regs_internal_truncate(uint64_t value, oemu_reg_width width);

/*
 * True when `cond` holds for the flags in `nzcv` (bits 31..28).
 *
 * Exhaustively testable: 16 conditions x 16 flag combinations. Worth doing,
 * because two properties are easy to break -- the odd/even pairs must be exact
 * complements, and 0b1111 (NV) must still mean "always".
 */
bool oemu_regs_internal_cond_holds(uint32_t nzcv, oemu_cond cond);

/*
 * AddWithCarry, per the ARM ARM pseudocode: computes x + y + carry_in at the
 * given width and derives all four flags. See the public header for how
 * subtraction maps onto it.
 */
oemu_alu_result oemu_regs_internal_add_with_carry(uint64_t x, uint64_t y, bool carry_in,
                                                  oemu_reg_width width);

OEMU_END_DECLS

#endif /* OEMU_SRC_REGS_INTERNAL_H */
