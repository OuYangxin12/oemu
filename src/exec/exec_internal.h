/*
 * Internal interface of the exec module -- NOT part of the public API.
 *
 * The interpreter's correctness concentrates in a small number of pure
 * functions: the shifted and extended operand forms with their imm6==0
 * carry quirks, the NZCV derivation for flag-setting logicals, and the
 * 128-bit multiply written as three 64-bit steps (production code may not use
 * __int128 under the project's -Wpedantic). Exposing them lets the tests
 * enumerate every edge case directly instead of constructing programs that
 * happen to reach them.
 *
 * oemu_exec_internal_dispatch is not pure -- it mutates cpu state -- but it is
 * exposed for the same reason: a white-box test can hand it a crafted
 * oemu_insn and pin down one opcode's semantics without assembling a program
 * around it.
 *
 * Rules for this pattern (see regs_internal.h): never installed, never
 * included by another module's public header, oemu_<module>_internal_ prefix.
 */
#ifndef OEMU_SRC_EXEC_INTERNAL_H
#define OEMU_SRC_EXEC_INTERNAL_H

#include "oemu/decode.h"
#include "oemu/exec.h"
#include "oemu/macros.h"
#include "oemu/memory.h"
#include "oemu/regs.h"
#include "oemu/status.h"
#include "oemu/sysenv.h"

#include <stdbool.h>
#include <stdint.h>

OEMU_BEGIN_DECLS

/* Result of applying a shifted-register operand modifier. `carry_valid` is
 * false exactly when the modifier leaves C unchanged (a shift by #0 LSL). */
typedef struct oemu_exec_shift_result {
  uint64_t value;
  bool carry_valid;
  bool carry;
} oemu_exec_shift_result;

/*
 * The shifted-register operand (logical and add/sub shifted forms). Beyond
 * the obvious shifts, the imm6 == 0 edges are the point, and each one behaves
 * differently:
 *
 *   LSL #0 -> the value passes through and C keeps its old value
 *   LSR #0 -> treated as a shift by the full width: result 0, C = the top bit
 *   ASR #0 -> treated as a shift by width-1: all-sign, C = the sign bit
 *   ROR #0 -> the architecture defines this as ASR #width-1, not "no rotate"
 *
 * `width` bounds both the operation and the carry position; a shift amount is
 * always below the width, which the decoder enforces.
 */
oemu_exec_shift_result oemu_exec_internal_shift_operand(uint64_t value, oemu_shift_type type,
                                                        unsigned amount, oemu_reg_width width);

/*
 * The extended-register operand (ADD extended, and every register-offset load
 * or store). `is_lsl` selects the UXTX-with-LSL variant: no extension at all,
 * just a left shift of the full 64-bit index. `shift` is pre-masked by the
 * decoder; the extension happens first, the shift second.
 */
uint64_t oemu_exec_internal_extend_operand(uint64_t index_value, oemu_extend_type type,
                                           unsigned shift, bool is_lsl);

/* NZ flags for a result truncated to `width`; Z is tested on the width, not 64. */
uint32_t oemu_exec_internal_nz(uint64_t result, oemu_reg_width width);

/* UMULH / SMULH without a 128-bit host type: three 32x32->64 partial
 * products plus explicit carry propagation. */
uint64_t oemu_exec_internal_umulh(uint64_t a, uint64_t b);
uint64_t oemu_exec_internal_smulh(uint64_t a, uint64_t b);

/* One-source data-processing helpers, all width-aware (CLZ/CLS count within
 * the width; RBIT reflects within the width; REVn swaps n-byte groups). */
uint64_t oemu_exec_internal_clz(uint64_t value, oemu_reg_width width);
uint64_t oemu_exec_internal_cls(uint64_t value, oemu_reg_width width);
uint64_t oemu_exec_internal_rbit(uint64_t value, oemu_reg_width width);
uint64_t oemu_exec_internal_rev(uint64_t value, oemu_reg_width width);
uint64_t oemu_exec_internal_rev16(uint64_t value, oemu_reg_width width);
uint64_t oemu_exec_internal_rev32(uint64_t value);

/*
 * Executes one already-decoded instruction (op != OEMU_OP_UNKNOWN), including
 * the PC update: branches land on insn->imm, everything else advances by 4.
 * Same precise-fault contract as oemu_exec_step.
 */
OEMU_NODISCARD oemu_status oemu_exec_internal_dispatch(oemu_cpu *cpu, oemu_memory *mem,
                                                       oemu_sysenv *env, const oemu_insn *insn);

OEMU_END_DECLS

#endif /* OEMU_SRC_EXEC_INTERNAL_H */
