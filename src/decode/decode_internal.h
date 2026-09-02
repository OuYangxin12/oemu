/*
 * Internal interface of the decoder -- NOT part of the public API.
 *
 * These are the pure bit-manipulation primitives the decoder is built from. They
 * are published here rather than made `static` because they are where the
 * genuinely tricky arithmetic lives, and they can be tested exhaustively:
 * sign-extension at every width, and DecodeBitMasks over its entire 2^19 input
 * space including the reserved combinations that must be rejected.
 *
 * Rules for this pattern:
 *   - never installed, never included by another module's public header;
 *   - symbols carry the oemu_decode_internal_ prefix to signal their status;
 *   - a change here is not a public API break.
 */
#ifndef OEMU_SRC_DECODE_INTERNAL_H
#define OEMU_SRC_DECODE_INTERNAL_H

#include "oemu/decode.h"
#include "oemu/macros.h"
#include "oemu/status.h"

#include <stdbool.h>
#include <stdint.h>

OEMU_BEGIN_DECLS

/* Extracts `width` bits starting at `lsb`. */
uint32_t oemu_decode_internal_bits(uint32_t word, unsigned lsb, unsigned width);

/*
 * Sign-extends the low `width` bits of `value` to 64 bits. `width` must be in
 * 1..64; anything else is a programming error and aborts.
 */
int64_t oemu_decode_internal_sign_extend(uint64_t value, unsigned width);

/* Rotates `value` right by `amount` within an `esize`-bit element. */
uint64_t oemu_decode_internal_ror(uint64_t value, unsigned amount, unsigned esize);

/*
 * DecodeBitMasks from the ARM ARM, as used by the immediate logical
 * instructions (AND/ORR/EOR/ANDS with an immediate).
 *
 * The N:imms:immr triple encodes a repeating bit pattern, not a plain constant:
 * `imms` selects the element size and the run length of ones, `immr` rotates the
 * element. Most of the 2^19 input space is unallocated, so this returns
 * OEMU_ERR_DECODE for the reserved combinations -- notably an all-ones element,
 * and N == 1 at 32-bit width.
 */
OEMU_NODISCARD oemu_status oemu_decode_internal_bit_masks(bool n, uint32_t imms, uint32_t immr,
                                                          oemu_reg_width width,
                                                          uint64_t *out_wmask);

/*
 * True when `option` and `amount` form a valid shift/extend for the
 * extended-register arithmetic forms: the amount must be 0..4.
 */
bool oemu_decode_internal_extend_amount_valid(unsigned amount);

OEMU_END_DECLS

#endif /* OEMU_SRC_DECODE_INTERNAL_H */
