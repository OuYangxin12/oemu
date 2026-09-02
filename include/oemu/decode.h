/*
 * A64 instruction decoder.
 *
 * Turns a 32-bit little-endian instruction word into a fully resolved
 * oemu_insn: opcode, operand registers, and every immediate already decoded,
 * sign-extended and scaled. The goal is that an executor never has to touch
 * encoding bits again -- all field extraction, the shift/extend selectors, the
 * logical-immediate bitmask expansion and the branch offset scaling happen here,
 * once.
 *
 * Scope is the ARMv8-A AArch64 user-mode subset described in the README: the
 * base integer instruction set plus SVC. Anything else is reported rather than
 * silently mapped onto something similar:
 *
 *   OEMU_ERR_DECODE       the word is not a valid A64 encoding (unallocated
 *                         slot, or a reserved field combination)
 *   OEMU_ERR_UNSUPPORTED  a real instruction that this subset excludes
 *                         (FP/SIMD, EL1+ system access, an excluded extension)
 *
 * That distinction matters when triaging: DECODE means the guest is wrong,
 * UNSUPPORTED means oemu is incomplete.
 */
#ifndef OEMU_DECODE_H
#define OEMU_DECODE_H

#include "oemu/macros.h"
#include "oemu/regs.h"
#include "oemu/status.h"

#include <stdbool.h>
#include <stdint.h>

OEMU_BEGIN_DECLS

/* --- operand modifiers ------------------------------------------------------- */

/* Shift applied to a register operand. */
typedef enum oemu_shift_type {
  OEMU_SHIFT_LSL = 0,
  OEMU_SHIFT_LSR = 1,
  OEMU_SHIFT_ASR = 2,
  OEMU_SHIFT_ROR = 3
} oemu_shift_type;

/*
 * Extension applied to a register operand, as encoded in the `option` field of
 * the extended-register and register-offset load/store forms. The enumerator
 * values are the encodings.
 */
typedef enum oemu_extend_type {
  OEMU_EXTEND_UXTB = 0,
  OEMU_EXTEND_UXTH = 1,
  OEMU_EXTEND_UXTW = 2,
  OEMU_EXTEND_UXTX = 3,
  OEMU_EXTEND_SXTB = 4,
  OEMU_EXTEND_SXTH = 5,
  OEMU_EXTEND_SXTW = 6,
  OEMU_EXTEND_SXTX = 7
} oemu_extend_type;

/* Access size of a load or store, as a log2 byte count. */
typedef enum oemu_mem_size {
  OEMU_MEM_BYTE = 0,
  OEMU_MEM_HALF = 1,
  OEMU_MEM_WORD = 2,
  OEMU_MEM_DWORD = 3
} oemu_mem_size;

/* How a load/store computes and writes back its address. */
typedef enum oemu_index_mode {
  OEMU_INDEX_NONE = 0, /* [Xn, #imm] -- no writeback */
  OEMU_INDEX_POST = 1, /* [Xn], #imm -- address is the old Xn */
  OEMU_INDEX_PRE = 2   /* [Xn, #imm]! -- address is the new Xn */
} oemu_index_mode;

/* --- opcodes ---------------------------------------------------------------- */

/*
 * One enumerator per distinct operation the executor must implement. Encoding
 * variants that differ only in how an operand is expressed collapse into the
 * same opcode: ADD immediate, ADD shifted-register and ADD extended-register
 * are all OEMU_OP_ADD, distinguished by oemu_insn::operand_kind. That keeps the
 * executor's switch proportional to semantics rather than to encoding space.
 */
typedef enum oemu_opcode {
  OEMU_OP_UNKNOWN = 0,

  /* arithmetic; the S forms set flags */
  OEMU_OP_ADD,
  OEMU_OP_ADDS,
  OEMU_OP_SUB,
  OEMU_OP_SUBS,
  OEMU_OP_ADC,
  OEMU_OP_ADCS,
  OEMU_OP_SBC,
  OEMU_OP_SBCS,

  /* logical */
  OEMU_OP_AND,
  OEMU_OP_ANDS,
  OEMU_OP_ORR,
  OEMU_OP_EOR,
  OEMU_OP_BIC,
  OEMU_OP_BICS,
  OEMU_OP_ORN,
  OEMU_OP_EON,

  /* move wide immediate */
  OEMU_OP_MOVZ,
  OEMU_OP_MOVN,
  OEMU_OP_MOVK,

  /* PC-relative address formation */
  OEMU_OP_ADR,
  OEMU_OP_ADRP,

  /* bitfield */
  OEMU_OP_SBFM,
  OEMU_OP_BFM,
  OEMU_OP_UBFM,
  OEMU_OP_EXTR,

  /* variable shift */
  OEMU_OP_LSLV,
  OEMU_OP_LSRV,
  OEMU_OP_ASRV,
  OEMU_OP_RORV,

  /* one-source */
  OEMU_OP_RBIT,
  OEMU_OP_REV16,
  OEMU_OP_REV32,
  OEMU_OP_REV,
  OEMU_OP_CLZ,
  OEMU_OP_CLS,

  /* divide and multiply */
  OEMU_OP_UDIV,
  OEMU_OP_SDIV,
  OEMU_OP_MADD,
  OEMU_OP_MSUB,
  OEMU_OP_SMADDL,
  OEMU_OP_SMSUBL,
  OEMU_OP_UMADDL,
  OEMU_OP_UMSUBL,
  OEMU_OP_SMULH,
  OEMU_OP_UMULH,

  /* conditional select and compare */
  OEMU_OP_CSEL,
  OEMU_OP_CSINC,
  OEMU_OP_CSINV,
  OEMU_OP_CSNEG,
  OEMU_OP_CCMP,
  OEMU_OP_CCMN,

  /* branches */
  OEMU_OP_B,
  OEMU_OP_BL,
  OEMU_OP_B_COND,
  OEMU_OP_BR,
  OEMU_OP_BLR,
  OEMU_OP_RET,
  OEMU_OP_CBZ,
  OEMU_OP_CBNZ,
  OEMU_OP_TBZ,
  OEMU_OP_TBNZ,

  /* loads and stores */
  OEMU_OP_LDR,
  OEMU_OP_STR,
  OEMU_OP_LDRS, /* sign-extending load; mem_size gives the loaded width */
  OEMU_OP_LDP,
  OEMU_OP_STP,
  OEMU_OP_LDPSW,
  OEMU_OP_LDXR, /* load-exclusive */
  OEMU_OP_STXR, /* store-exclusive */
  OEMU_OP_LDAR, /* load-acquire */
  OEMU_OP_STLR, /* store-release */

  /* system and exception */
  OEMU_OP_SVC,
  OEMU_OP_BRK,
  OEMU_OP_HLT,
  OEMU_OP_NOP,
  OEMU_OP_HINT,    /* YIELD/WFE/WFI/SEV: architecturally a hint, a NOP here */
  OEMU_OP_BARRIER, /* DMB/DSB/ISB: ordering is trivially satisfied */
  OEMU_OP_MRS,
  OEMU_OP_MSR
} oemu_opcode;

/*
 * How the instruction's second source operand is expressed. The executor
 * switches on this to know which oemu_insn fields carry meaning.
 */
typedef enum oemu_operand_kind {
  OEMU_OPERAND_NONE = 0,     /* no second source */
  OEMU_OPERAND_IMM,          /* imm */
  OEMU_OPERAND_REG,          /* rm */
  OEMU_OPERAND_REG_SHIFTED,  /* rm, shift_type #shift_amount */
  OEMU_OPERAND_REG_EXTENDED, /* rm, extend_type #shift_amount */
  OEMU_OPERAND_MEM           /* a load/store address operand */
} oemu_operand_kind;

/* --- the decoded instruction ------------------------------------------------ */

/*
 * A decoded A64 instruction. Every immediate here is final: sign-extended and
 * scaled as the encoding requires, so the executor performs no bit surgery.
 *
 * Register numbers are raw 5-bit encodings, so 31 still means "XZR or SP". Which
 * one applies is given by `rn_is_sp_form` / `rd_is_sp_form`, because it is a
 * property of the encoding and only the decoder knows it -- see oemu/regs.h for
 * the two accessor families this selects between.
 */
typedef struct oemu_insn {
  uint32_t word; /* the raw instruction, kept for diagnostics */
  oemu_opcode op;
  oemu_operand_kind operand_kind;
  oemu_reg_width width; /* operand width from the sf bit */

  unsigned rd;  /* destination, or the first source of a store */
  unsigned rn;  /* first source, or the base register of a load/store */
  unsigned rm;  /* second source register */
  unsigned ra;  /* third source, for the multiply-accumulate forms */
  unsigned rt2; /* second transfer register, for the pair forms */

  /*
   * Sign-extended where the encoding is signed: branch offsets, load/store
   * immediates and CCMP's comparison value. Unsigned otherwise.
   */
  int64_t imm;

  /* Set when `imm` should be read as an unsigned bit pattern (logical
   * immediates, MOVZ/MOVN/MOVK, shift amounts). */
  uint64_t uimm;

  oemu_shift_type shift_type;
  oemu_extend_type extend_type;
  unsigned shift_amount;

  oemu_cond cond; /* for B.cond, CSEL and CCMP */

  /* load/store specifics */
  oemu_mem_size mem_size;
  oemu_index_mode index_mode;
  bool is_signed_load; /* LDRS*: sign-extend the loaded value */
  bool extend_is_lsl;  /* register-offset form uses a plain LSL, not an extend */

  bool sets_flags;    /* the S variant: write NZCV */
  bool rn_is_sp_form; /* Rn == 31 means SP, not XZR */
  bool rd_is_sp_form; /* Rd == 31 means SP, not the discard */
  unsigned bit_pos;   /* TBZ/TBNZ bit position */
  uint32_t sysreg;    /* MRS/MSR: the encoded op0..op2 selector */
} oemu_insn;

/*
 * Decodes one instruction word.
 *
 * `pc` is the address the word was fetched from; PC-relative operands (ADR,
 * ADRP, the branches and LDR literal) are resolved against it, so `imm` holds a
 * final target address rather than an offset.
 *
 * Returns OEMU_OK on success, OEMU_ERR_INVALID_ARG if `out` is NULL,
 * OEMU_ERR_DECODE for an invalid encoding, or OEMU_ERR_UNSUPPORTED for a valid
 * instruction outside the emulated subset. On any error `out` is zeroed with
 * `op` left as OEMU_OP_UNKNOWN, so a caller that ignores the status cannot act
 * on stale fields.
 */
OEMU_NODISCARD oemu_status oemu_decode(uint32_t word, uint64_t pc, oemu_insn *out);

/* Stable, never-NULL mnemonic for an opcode; "unknown" for OEMU_OP_UNKNOWN. */
const char *oemu_opcode_name(oemu_opcode op);

OEMU_END_DECLS

#endif /* OEMU_DECODE_H */
