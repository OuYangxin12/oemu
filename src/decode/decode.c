/*
 * A64 decoder.
 *
 * Structure follows the ARM ARM's own top-level classification, which keys on
 * bits 28:25 ("op0"). Mirroring the manual's decode tree deliberately: it makes
 * each function checkable against a specific published table instead of against
 * someone's mental model of the encoding space.
 *
 *   op0        group
 *   ---------  ---------------------------------------------
 *   100x       data processing -- immediate
 *   101x       branches, exception generation, system
 *   x1x0       loads and stores
 *   x101       data processing -- register
 *   x111       data processing -- SIMD/FP  (outside this subset)
 *   0000       reserved / unallocated
 *
 * Everything unallocated returns OEMU_ERR_DECODE, and everything real but out of
 * scope returns OEMU_ERR_UNSUPPORTED. Nothing falls through to a plausible
 * neighbouring instruction, because a decoder that guesses produces guest
 * misbehaviour that is far harder to diagnose than a clean refusal.
 */
#include "oemu/decode.h"

#include "oemu/check.h"
#include "oemu/regs.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "decode_internal.h"

/* --- bit primitives (exposed via decode_internal.h) -------------------------- */

uint32_t oemu_decode_internal_bits(uint32_t word, unsigned lsb, unsigned width) {
  OEMU_REQUIRE(width >= 1U && width <= 32U, "bit field width must be 1..32");
  OEMU_REQUIRE(lsb + width <= 32U, "bit field must lie within the instruction word");
  if (width == 32U) {
    return word;
  }
  return (word >> lsb) & ((UINT32_C(1) << width) - 1U);
}

int64_t oemu_decode_internal_sign_extend(uint64_t value, unsigned width) {
  OEMU_REQUIRE(width >= 1U && width <= 64U, "sign-extend width must be 1..64");
  if (width == 64U) {
    return (int64_t)value;
  }
  const uint64_t mask = (UINT64_C(1) << width) - 1U;
  const uint64_t sign = UINT64_C(1) << (width - 1U);
  const uint64_t masked = value & mask;
  /* Build the result as unsigned and convert once: shifting or negating a
   * signed value into the sign bit would be undefined behaviour. */
  if ((masked & sign) != 0U) {
    return (int64_t)(masked | ~mask);
  }
  return (int64_t)masked;
}

uint64_t oemu_decode_internal_ror(uint64_t value, unsigned amount, unsigned esize) {
  OEMU_REQUIRE(esize >= 1U && esize <= 64U, "rotate element size must be 1..64");
  const unsigned shift = amount % esize;
  const uint64_t mask = (esize == 64U) ? ~UINT64_C(0) : ((UINT64_C(1) << esize) - 1U);
  const uint64_t masked = value & mask;
  if (shift == 0U) {
    return masked;
  }
  return ((masked >> shift) | (masked << (esize - shift))) & mask;
}

oemu_status oemu_decode_internal_bit_masks(bool n, uint32_t imms, uint32_t immr,
                                           oemu_reg_width width, uint64_t *out_wmask) {
  if (out_wmask == NULL) {
    return OEMU_ERR_INVALID_ARG;
  }

  const unsigned reg_bits = (width == OEMU_REG_W32) ? 32U : 64U;

  /*
   * `len` is the position of the highest set bit of N:NOT(imms), and selects the
   * element size as 2^len. len == 0 would mean a 1-bit element, which is not
   * encodable, so it is reserved.
   */
  const uint32_t combined = ((n ? 1U : 0U) << 6U) | ((~imms) & 0x3FU);
  int len = -1;
  for (int bit = 6; bit >= 0; --bit) {
    if ((combined & (UINT32_C(1) << (unsigned)bit)) != 0U) {
      len = bit;
      break;
    }
  }
  if (len < 1) {
    return OEMU_ERR_DECODE;
  }

  const unsigned esize = 1U << (unsigned)len;
  if (esize > reg_bits) {
    /* Implied by len <= 5 at 32-bit width, i.e. N must be 0 there. */
    return OEMU_ERR_DECODE;
  }

  const uint32_t levels = esize - 1U;
  const uint32_t s = imms & levels;
  const uint32_t r = immr & levels;

  /*
   * s == levels would make the element all ones, which the immediate forms
   * cannot represent -- that pattern is what MOVN/ORR with XZR is for.
   */
  if (s == levels) {
    return OEMU_ERR_DECODE;
  }

  /* A run of s+1 ones, rotated right by r within the element. */
  uint64_t element = (s + 1U == 64U) ? ~UINT64_C(0) : ((UINT64_C(1) << (s + 1U)) - 1U);
  element = oemu_decode_internal_ror(element, r, esize);

  /* Replicate the element across the register width. */
  uint64_t mask = 0;
  for (unsigned pos = 0; pos < reg_bits; pos += esize) {
    mask |= element << pos;
  }
  if (reg_bits == 32U) {
    mask &= UINT64_C(0xFFFFFFFF);
  }

  *out_wmask = mask;
  return OEMU_OK;
}

bool oemu_decode_internal_extend_amount_valid(unsigned amount) {
  return amount <= 4U;
}

/* --- shared field accessors -------------------------------------------------- */
/*
 * Named after the ARM ARM's field names so a reader can follow the manual's
 * tables directly.
 */

#define BITS(w, lsb, n) oemu_decode_internal_bits((w), (lsb), (n))
#define BIT(w, pos)     oemu_decode_internal_bits((w), (pos), 1U)

static unsigned field_rd(uint32_t word) {
  return BITS(word, 0, 5);
}
static unsigned field_rn(uint32_t word) {
  return BITS(word, 5, 5);
}
static unsigned field_rm(uint32_t word) {
  return BITS(word, 16, 5);
}
static unsigned field_ra(uint32_t word) {
  return BITS(word, 10, 5);
}

static oemu_reg_width width_from_sf(uint32_t word) {
  return (BIT(word, 31) != 0U) ? OEMU_REG_W64 : OEMU_REG_W32;
}

/*
 * Field-to-enum conversions. These exist as functions taking a value rather than
 * as casts applied directly to BITS(...), because -Wbad-function-cast rejects
 * casting a function result straight to an enum. Routing through a parameter
 * also gives each conversion one place to assert its domain.
 */
static oemu_cond to_cond(uint32_t value) {
  OEMU_REQUIRE(value <= 0xFU, "condition field must be 4 bits");
  return (oemu_cond)value;
}

static oemu_shift_type to_shift_type(uint32_t value) {
  OEMU_REQUIRE(value <= 0x3U, "shift type field must be 2 bits");
  return (oemu_shift_type)value;
}

static oemu_extend_type to_extend_type(uint32_t value) {
  OEMU_REQUIRE(value <= 0x7U, "extend type field must be 3 bits");
  return (oemu_extend_type)value;
}

static oemu_mem_size to_mem_size(uint32_t value) {
  OEMU_REQUIRE(value <= 0x3U, "access size field must be 2 bits");
  return (oemu_mem_size)value;
}

/*
 * The add/subtract encodings select from four operations using two independent
 * bits: bit 30 chooses subtract and bit 29 chooses to write flags. Written as
 * nested conditionals this is unreadable and trips readability's nesting rule,
 * and the same mapping is needed by three different encoding forms, so it is
 * spelled out once as a table.
 */
static oemu_opcode addsub_opcode(bool is_sub, bool sets_flags) {
  static const oemu_opcode table[2][2] = {
      {/* not subtract */ OEMU_OP_ADD, OEMU_OP_ADDS},
      {/* subtract     */ OEMU_OP_SUB, OEMU_OP_SUBS},
  };
  return table[is_sub ? 1 : 0][sets_flags ? 1 : 0];
}

/* The same two-bit shape drives the carry-chain operations. */
static oemu_opcode addsub_carry_opcode(bool is_sub, bool sets_flags) {
  static const oemu_opcode table[2][2] = {
      {/* not subtract */ OEMU_OP_ADC, OEMU_OP_ADCS},
      {/* subtract     */ OEMU_OP_SBC, OEMU_OP_SBCS},
  };
  return table[is_sub ? 1 : 0][sets_flags ? 1 : 0];
}

/*
 * `opc` in the two remaining multi-opcode encodings is a plain selector rather
 * than two independent bits, so it maps through a switch. Both have one
 * unallocated value that the caller has already rejected, which is why the
 * default arm names the last operation instead of returning a status.
 */
static oemu_opcode move_wide_opcode(uint32_t opc) {
  switch (opc) {
    case 0x0:
      return OEMU_OP_MOVN;
    case 0x2:
      return OEMU_OP_MOVZ;
    default:
      return OEMU_OP_MOVK;
  }
}

static oemu_opcode bitfield_opcode(uint32_t opc) {
  switch (opc) {
    case 0x0:
      return OEMU_OP_SBFM;
    case 0x1:
      return OEMU_OP_BFM;
    default:
      return OEMU_OP_UBFM;
  }
}

static void insn_reset(oemu_insn *insn, uint32_t word) {
  memset(insn, 0, sizeof(*insn));
  insn->word = word;
  insn->op = OEMU_OP_UNKNOWN;
  insn->width = OEMU_REG_W64;
}

/* --- data processing: immediate (op0 == 100x) -------------------------------- */

static oemu_status decode_pc_rel_addr(uint32_t word, uint64_t pc, oemu_insn *insn) {
  const uint64_t immlo = BITS(word, 29, 2);
  const uint64_t immhi = BITS(word, 5, 19);
  const int64_t offset = oemu_decode_internal_sign_extend((immhi << 2U) | immlo, 21U);

  insn->rd = field_rd(word);
  insn->operand_kind = OEMU_OPERAND_IMM;
  insn->width = OEMU_REG_W64; /* both forms always write a 64-bit register */

  if (BIT(word, 31) != 0U) {
    /* ADRP: the offset is in 4 KiB pages and the base PC is page-aligned. */
    insn->op = OEMU_OP_ADRP;
    const uint64_t page = pc & ~UINT64_C(0xFFF);
    insn->imm = (int64_t)(page + ((uint64_t)offset << 12U));
  } else {
    insn->op = OEMU_OP_ADR;
    insn->imm = (int64_t)(pc + (uint64_t)offset);
  }
  return OEMU_OK;
}

static oemu_status decode_addsub_imm(uint32_t word, oemu_insn *insn) {
  const uint32_t shift = BITS(word, 22, 2);
  /* Only LSL #0 and LSL #12 exist; the other two encodings are unallocated. */
  if (shift > 1U) {
    return OEMU_ERR_DECODE;
  }

  const uint64_t imm12 = BITS(word, 10, 12);
  const bool is_sub = BIT(word, 30) != 0U;
  const bool sets_flags = BIT(word, 29) != 0U;

  insn->width = width_from_sf(word);
  insn->rd = field_rd(word);
  insn->rn = field_rn(word);
  insn->operand_kind = OEMU_OPERAND_IMM;
  insn->uimm = (shift != 0U) ? (imm12 << 12U) : imm12;
  insn->imm = (int64_t)insn->uimm;
  insn->sets_flags = sets_flags;
  insn->op = addsub_opcode(is_sub, sets_flags);

  /*
   * Rn is the SP form here: `add sp, sp, #16` is a real and common encoding.
   * Rd is the SP form only when the instruction does not set flags -- for ADDS
   * and SUBS, Rd == 31 is the discard that makes CMP and CMN work.
   */
  insn->rn_is_sp_form = true;
  insn->rd_is_sp_form = !sets_flags;
  return OEMU_OK;
}

static oemu_status decode_logical_imm(uint32_t word, oemu_insn *insn) {
  const bool n = BIT(word, 22) != 0U;
  const oemu_reg_width width = width_from_sf(word);

  /* N must be 0 for the 32-bit forms; bit_masks rejects the combination. */
  uint64_t wmask = 0;
  const oemu_status status =
      oemu_decode_internal_bit_masks(n, BITS(word, 10, 6), BITS(word, 16, 6), width, &wmask);
  if (status != OEMU_OK) {
    return status;
  }

  insn->width = width;
  insn->rd = field_rd(word);
  insn->rn = field_rn(word);
  insn->operand_kind = OEMU_OPERAND_IMM;
  insn->uimm = wmask;
  insn->imm = (int64_t)wmask;

  switch (BITS(word, 29, 2)) {
    case 0x0:
      insn->op = OEMU_OP_AND;
      break;
    case 0x1:
      insn->op = OEMU_OP_ORR;
      break;
    case 0x2:
      insn->op = OEMU_OP_EOR;
      break;
    default:
      insn->op = OEMU_OP_ANDS;
      insn->sets_flags = true;
      break;
  }
  /* ORR/AND/EOR immediate may write SP (`mov sp, x0` is ORR); ANDS may not. */
  insn->rd_is_sp_form = !insn->sets_flags;
  return OEMU_OK;
}

static oemu_status decode_move_wide(uint32_t word, oemu_insn *insn) {
  const uint32_t hw = BITS(word, 21, 2);
  const oemu_reg_width width = width_from_sf(word);

  /* At 32-bit width only hw 0 and 1 exist: a shift of 32 or 48 has no meaning. */
  if (width == OEMU_REG_W32 && hw > 1U) {
    return OEMU_ERR_DECODE;
  }

  const uint32_t opc = BITS(word, 29, 2);
  if (opc == 0x1U) {
    return OEMU_ERR_DECODE; /* unallocated */
  }

  insn->width = width;
  insn->rd = field_rd(word);
  insn->operand_kind = OEMU_OPERAND_IMM;
  insn->shift_amount = hw * 16U;
  insn->uimm = (uint64_t)BITS(word, 5, 16) << insn->shift_amount;
  insn->imm = (int64_t)insn->uimm;
  insn->op = move_wide_opcode(opc);
  return OEMU_OK;
}

static oemu_status decode_bitfield(uint32_t word, oemu_insn *insn) {
  const bool n = BIT(word, 22) != 0U;
  const oemu_reg_width width = width_from_sf(word);
  const uint32_t opc = BITS(word, 29, 2);

  if (opc == 0x3U) {
    return OEMU_ERR_DECODE; /* unallocated */
  }
  /* N must track sf: a 64-bit bitfield op cannot have N == 0. */
  if ((width == OEMU_REG_W64) != n) {
    return OEMU_ERR_DECODE;
  }

  const uint32_t immr = BITS(word, 16, 6);
  const uint32_t imms = BITS(word, 10, 6);
  const unsigned reg_bits = (width == OEMU_REG_W32) ? 32U : 64U;
  /* Both fields index a bit position, so neither may reach the register width. */
  if (immr >= reg_bits || imms >= reg_bits) {
    return OEMU_ERR_DECODE;
  }

  insn->width = width;
  insn->rd = field_rd(word);
  insn->rn = field_rn(word);
  insn->operand_kind = OEMU_OPERAND_IMM;
  insn->uimm = ((uint64_t)immr << 8U) | imms; /* immr in 15:8, imms in 7:0 */
  insn->imm = (int64_t)insn->uimm;
  insn->shift_amount = immr;
  insn->bit_pos = imms;
  insn->op = bitfield_opcode(opc);
  return OEMU_OK;
}

static oemu_status decode_extract(uint32_t word, oemu_insn *insn) {
  const bool n = BIT(word, 22) != 0U;
  const oemu_reg_width width = width_from_sf(word);

  if (BITS(word, 29, 2) != 0U || BIT(word, 21) != 0U) {
    return OEMU_ERR_DECODE;
  }
  if ((width == OEMU_REG_W64) != n) {
    return OEMU_ERR_DECODE;
  }

  const uint32_t imms = BITS(word, 10, 6);
  const unsigned reg_bits = (width == OEMU_REG_W32) ? 32U : 64U;
  if (imms >= reg_bits) {
    return OEMU_ERR_DECODE;
  }

  insn->width = width;
  insn->rd = field_rd(word);
  insn->rn = field_rn(word);
  insn->rm = field_rm(word);
  insn->operand_kind = OEMU_OPERAND_REG;
  insn->shift_amount = imms;
  insn->uimm = imms;
  insn->imm = (int64_t)imms;
  insn->op = OEMU_OP_EXTR;
  return OEMU_OK;
}

static oemu_status decode_dp_immediate(uint32_t word, uint64_t pc, oemu_insn *insn) {
  switch (BITS(word, 23, 3)) {
    case 0x0:
    case 0x1:
      return decode_pc_rel_addr(word, pc, insn);
    case 0x2:
    case 0x3:
      return decode_addsub_imm(word, insn);
    case 0x4:
      return decode_logical_imm(word, insn);
    case 0x5:
      return decode_move_wide(word, insn);
    case 0x6:
      return decode_bitfield(word, insn);
    default:
      return decode_extract(word, insn);
  }
}

/* --- branches, exceptions and system (op0 == 101x) -------------------------- */

static oemu_status decode_uncond_branch_imm(uint32_t word, uint64_t pc, oemu_insn *insn) {
  const int64_t offset = oemu_decode_internal_sign_extend(BITS(word, 0, 26), 26U);
  insn->op = (BIT(word, 31) != 0U) ? OEMU_OP_BL : OEMU_OP_B;
  insn->operand_kind = OEMU_OPERAND_IMM;
  /* Offsets are in instructions, so scale by 4 and resolve against PC. */
  insn->imm = (int64_t)(pc + ((uint64_t)offset << 2U));
  return OEMU_OK;
}

static oemu_status decode_cond_branch_imm(uint32_t word, uint64_t pc, oemu_insn *insn) {
  if (BIT(word, 24) != 0U || BIT(word, 4) != 0U) {
    return OEMU_ERR_DECODE;
  }
  const int64_t offset = oemu_decode_internal_sign_extend(BITS(word, 5, 19), 19U);
  insn->op = OEMU_OP_B_COND;
  insn->operand_kind = OEMU_OPERAND_IMM;
  insn->cond = to_cond(BITS(word, 0, 4));
  insn->imm = (int64_t)(pc + ((uint64_t)offset << 2U));
  return OEMU_OK;
}

static oemu_status decode_compare_branch(uint32_t word, uint64_t pc, oemu_insn *insn) {
  const int64_t offset = oemu_decode_internal_sign_extend(BITS(word, 5, 19), 19U);
  insn->width = width_from_sf(word);
  insn->rt2 = field_rd(word); /* the tested register sits in the Rt field */
  insn->rd = field_rd(word);
  insn->operand_kind = OEMU_OPERAND_IMM;
  insn->imm = (int64_t)(pc + ((uint64_t)offset << 2U));
  insn->op = (BIT(word, 24) != 0U) ? OEMU_OP_CBNZ : OEMU_OP_CBZ;
  return OEMU_OK;
}

static oemu_status decode_test_branch(uint32_t word, uint64_t pc, oemu_insn *insn) {
  const int64_t offset = oemu_decode_internal_sign_extend(BITS(word, 5, 14), 14U);
  /* The bit position is split: b5 is bit 31, b40 is bits 23:19. */
  const unsigned bit_pos = (BIT(word, 31) << 5U) | BITS(word, 19, 5);

  insn->width = (BIT(word, 31) != 0U) ? OEMU_REG_W64 : OEMU_REG_W32;
  insn->rd = field_rd(word);
  insn->rt2 = field_rd(word);
  insn->bit_pos = bit_pos;
  insn->operand_kind = OEMU_OPERAND_IMM;
  insn->imm = (int64_t)(pc + ((uint64_t)offset << 2U));
  insn->op = (BIT(word, 24) != 0U) ? OEMU_OP_TBNZ : OEMU_OP_TBZ;
  return OEMU_OK;
}

static oemu_status decode_exception(uint32_t word, oemu_insn *insn) {
  const uint32_t opc = BITS(word, 21, 3);
  const uint32_t op2 = BITS(word, 2, 3);
  const uint32_t ll = BITS(word, 0, 2);

  if (op2 != 0U) {
    return OEMU_ERR_DECODE;
  }

  insn->operand_kind = OEMU_OPERAND_IMM;
  insn->uimm = BITS(word, 5, 16);
  insn->imm = (int64_t)insn->uimm;

  if (opc == 0x0U && ll == 0x1U) {
    /* SVC: the one exception-generating instruction the subset implements. */
    insn->op = OEMU_OP_SVC;
    return OEMU_OK;
  }
  if (opc == 0x1U && ll == 0x0U) {
    insn->op = OEMU_OP_BRK;
    return OEMU_OK;
  }
  if (opc == 0x2U && ll == 0x0U) {
    insn->op = OEMU_OP_HLT;
    return OEMU_OK;
  }
  /* HVC, SMC and the DCPS forms are all EL1+ operations. */
  if (opc == 0x0U && (ll == 0x2U || ll == 0x3U)) {
    return OEMU_ERR_UNSUPPORTED;
  }
  if (opc == 0x5U) {
    return OEMU_ERR_UNSUPPORTED;
  }
  return OEMU_ERR_DECODE;
}

static oemu_status decode_hints_and_barriers(uint32_t word, oemu_insn *insn) {
  const uint32_t crn = BITS(word, 12, 4);
  const uint32_t crm = BITS(word, 8, 4);
  const uint32_t op2 = BITS(word, 5, 3);

  if (crn == 0x2U) {
    /* Hint space. NOP is hint #0; the rest (YIELD, WFE, WFI, SEV, SEVL) have no
     * observable effect on a single-threaded user-mode model. */
    insn->uimm = (crm << 3U) | op2;
    insn->imm = (int64_t)insn->uimm;
    insn->op = (insn->uimm == 0U) ? OEMU_OP_NOP : OEMU_OP_HINT;
    return OEMU_OK;
  }
  if (crn == 0x3U) {
    /* Barriers: DMB, DSB, ISB, CLREX. Ordering is trivially satisfied by an
     * in-order single-threaded interpreter, so they decode to a no-op marker
     * rather than being rejected. */
    insn->uimm = crm;
    insn->imm = (int64_t)crm;
    insn->op = OEMU_OP_BARRIER;
    return OEMU_OK;
  }
  /* Cache and TLB maintenance (CRn 7/8) needs a memory model to mean anything. */
  return OEMU_ERR_UNSUPPORTED;
}

static oemu_status decode_system(uint32_t word, oemu_insn *insn) {
  const uint32_t l = BIT(word, 21);
  const uint32_t op0 = BITS(word, 19, 2);
  const uint32_t rt = field_rd(word);

  if (op0 == 0x0U) {
    /* MSR (immediate) and the hint/barrier space; Rt must be 31. */
    if (rt != 31U) {
      return OEMU_ERR_DECODE;
    }
    if (l != 0U) {
      return OEMU_ERR_DECODE;
    }
    return decode_hints_and_barriers(word, insn);
  }

  if (op0 == 0x1U) {
    /* SYS/SYSL: system instructions such as cache maintenance by address. */
    return OEMU_ERR_UNSUPPORTED;
  }

  /*
   * MRS/MSR (register). The full op0:op1:CRn:CRm:op2 selector is kept so an
   * executor can implement individual registers (TPIDR_EL0, NZCV) and refuse the
   * rest; validating which registers exist is not the decoder's job.
   */
  insn->sysreg = BITS(word, 5, 15);
  insn->rd = rt;
  insn->rt2 = rt;
  insn->operand_kind = OEMU_OPERAND_NONE;
  insn->op = (l != 0U) ? OEMU_OP_MRS : OEMU_OP_MSR;
  return OEMU_OK;
}

static oemu_status decode_uncond_branch_reg(uint32_t word, oemu_insn *insn) {
  const uint32_t opc = BITS(word, 21, 4);
  const uint32_t op2 = BITS(word, 16, 5);
  const uint32_t op3 = BITS(word, 10, 6);
  const uint32_t op4 = BITS(word, 0, 5);

  if (op2 != 0x1FU || op3 != 0U || op4 != 0U) {
    /* Non-zero op3/op4 selects the pointer-authentication variants (BRAA and
     * friends), which this subset excludes. */
    if (op3 != 0U) {
      return OEMU_ERR_UNSUPPORTED;
    }
    return OEMU_ERR_DECODE;
  }

  insn->rn = field_rn(word);
  insn->operand_kind = OEMU_OPERAND_REG;
  switch (opc) {
    case 0x0:
      insn->op = OEMU_OP_BR;
      return OEMU_OK;
    case 0x1:
      insn->op = OEMU_OP_BLR;
      return OEMU_OK;
    case 0x2:
      insn->op = OEMU_OP_RET;
      return OEMU_OK;
    case 0x4: /* ERET */
    case 0x5: /* DRPS */
      return OEMU_ERR_UNSUPPORTED;
    default:
      return OEMU_ERR_DECODE;
  }
}

/*
 * Branches, exception generation and system instructions.
 *
 * The ARM ARM keys this group on bits 31:29 ("op0") together with bits 25:22.
 * Getting op0 wrong here is unusually costly: the group holds B, BL, RET, SVC
 * and the whole system space, so a bad split silently turns system instructions
 * into branches. The values below were verified against assembled encodings.
 *
 *   op0   group
 *   ----  -------------------------------------------------------
 *   010   conditional branch immediate            (B.cond)
 *   000   unconditional branch immediate          (B)
 *   100   unconditional branch immediate + link   (BL)
 *   001   compare-and-branch / test-and-branch    (CBZ, TBZ)
 *   101   compare-and-branch / test-and-branch    (CBNZ, TBNZ)
 *   110   exception, system, branch-to-register   (SVC, MRS, BR, RET)
 */
static oemu_status decode_branch_system(uint32_t word, uint64_t pc, oemu_insn *insn) {
  const uint32_t op0 = BITS(word, 29, 3);

  switch (op0) {
    case 0x0: /* B */
    case 0x4: /* BL */
      return decode_uncond_branch_imm(word, pc, insn);

    case 0x2: /* B.cond */
      if (BIT(word, 25) != 0U) {
        return OEMU_ERR_DECODE;
      }
      return decode_cond_branch_imm(word, pc, insn);

    case 0x1:
    case 0x5:
      /* Bit 25 separates compare-and-branch from test-and-branch. */
      if (BIT(word, 25) == 0U) {
        return decode_compare_branch(word, pc, insn);
      }
      return decode_test_branch(word, pc, insn);

    case 0x6:
      /* Exception generation, the system space, and the register branches. */
      if (BIT(word, 25) != 0U) {
        return decode_uncond_branch_reg(word, insn);
      }
      /*
       * Bit 24 alone separates exception generation from the system space. The
       * `opc` field (23:21) belongs to decode_exception -- constraining it here
       * would reject HLT, whose opc is 0b010.
       */
      if (BIT(word, 24) == 0U) {
        return decode_exception(word, insn);
      }
      return decode_system(word, insn);

    default:
      /* op0 == 011 and 111 are unallocated in this group. */
      return OEMU_ERR_DECODE;
  }
}

/* --- data processing: register (op0 == x101) -------------------------------- */

static oemu_status decode_logical_shifted(uint32_t word, oemu_insn *insn) {
  const uint32_t opc = BITS(word, 29, 2);
  const bool negate = BIT(word, 21) != 0U;
  const uint32_t shift = BITS(word, 22, 2);
  const uint32_t amount = BITS(word, 10, 6);
  const oemu_reg_width width = width_from_sf(word);

  /* A 32-bit shift amount cannot reach 32. */
  if (width == OEMU_REG_W32 && amount >= 32U) {
    return OEMU_ERR_DECODE;
  }

  insn->width = width;
  insn->rd = field_rd(word);
  insn->rn = field_rn(word);
  insn->rm = field_rm(word);
  insn->shift_type = to_shift_type(shift);
  insn->shift_amount = amount;
  insn->operand_kind = (amount != 0U) ? OEMU_OPERAND_REG_SHIFTED : OEMU_OPERAND_REG;

  switch (opc) {
    case 0x0:
      insn->op = negate ? OEMU_OP_BIC : OEMU_OP_AND;
      break;
    case 0x1:
      insn->op = negate ? OEMU_OP_ORN : OEMU_OP_ORR;
      break;
    case 0x2:
      insn->op = negate ? OEMU_OP_EON : OEMU_OP_EOR;
      break;
    default:
      insn->op = negate ? OEMU_OP_BICS : OEMU_OP_ANDS;
      insn->sets_flags = true;
      break;
  }
  return OEMU_OK;
}

static oemu_status decode_addsub_shifted(uint32_t word, oemu_insn *insn) {
  const uint32_t shift = BITS(word, 22, 2);
  const uint32_t amount = BITS(word, 10, 6);
  const oemu_reg_width width = width_from_sf(word);

  if (shift == 0x3U) {
    return OEMU_ERR_DECODE; /* ROR is not available in this form */
  }
  if (width == OEMU_REG_W32 && amount >= 32U) {
    return OEMU_ERR_DECODE;
  }

  const bool is_sub = BIT(word, 30) != 0U;
  const bool sets_flags = BIT(word, 29) != 0U;

  insn->width = width;
  insn->rd = field_rd(word);
  insn->rn = field_rn(word);
  insn->rm = field_rm(word);
  insn->shift_type = to_shift_type(shift);
  insn->shift_amount = amount;
  insn->operand_kind = (amount != 0U) ? OEMU_OPERAND_REG_SHIFTED : OEMU_OPERAND_REG;
  insn->sets_flags = sets_flags;
  insn->op = addsub_opcode(is_sub, sets_flags);
  return OEMU_OK;
}

static oemu_status decode_addsub_extended(uint32_t word, oemu_insn *insn) {
  if (BITS(word, 22, 2) != 0U) {
    return OEMU_ERR_DECODE;
  }
  const unsigned amount = BITS(word, 10, 3);
  if (!oemu_decode_internal_extend_amount_valid(amount)) {
    return OEMU_ERR_DECODE;
  }

  const bool is_sub = BIT(word, 30) != 0U;
  const bool sets_flags = BIT(word, 29) != 0U;

  insn->width = width_from_sf(word);
  insn->rd = field_rd(word);
  insn->rn = field_rn(word);
  insn->rm = field_rm(word);
  insn->extend_type = to_extend_type(BITS(word, 13, 3));
  insn->shift_amount = amount;
  insn->operand_kind = OEMU_OPERAND_REG_EXTENDED;
  insn->sets_flags = sets_flags;
  insn->op = addsub_opcode(is_sub, sets_flags);

  /* This form reads SP as the base, and writes it when it does not set flags. */
  insn->rn_is_sp_form = true;
  insn->rd_is_sp_form = !sets_flags;
  return OEMU_OK;
}

static oemu_status decode_addsub_carry(uint32_t word, oemu_insn *insn) {
  if (BITS(word, 10, 6) != 0U) {
    return OEMU_ERR_DECODE;
  }
  const bool is_sub = BIT(word, 30) != 0U;
  const bool sets_flags = BIT(word, 29) != 0U;

  insn->width = width_from_sf(word);
  insn->rd = field_rd(word);
  insn->rn = field_rn(word);
  insn->rm = field_rm(word);
  insn->operand_kind = OEMU_OPERAND_REG;
  insn->sets_flags = sets_flags;
  insn->op = addsub_carry_opcode(is_sub, sets_flags);
  return OEMU_OK;
}

static oemu_status decode_cond_compare(uint32_t word, oemu_insn *insn) {
  if (BIT(word, 29) == 0U || BIT(word, 10) != 0U || BIT(word, 4) != 0U) {
    return OEMU_ERR_DECODE;
  }
  const bool is_imm = BIT(word, 11) != 0U;

  insn->width = width_from_sf(word);
  insn->rn = field_rn(word);
  insn->cond = to_cond(BITS(word, 12, 4));
  /* The 4-bit nzcv field supplies the flags to use when the condition fails. */
  insn->uimm = BITS(word, 0, 4);
  insn->sets_flags = true;

  if (is_imm) {
    insn->operand_kind = OEMU_OPERAND_IMM;
    insn->imm = (int64_t)BITS(word, 16, 5);
  } else {
    insn->operand_kind = OEMU_OPERAND_REG;
    insn->rm = field_rm(word);
  }
  insn->op = (BIT(word, 30) != 0U) ? OEMU_OP_CCMP : OEMU_OP_CCMN;
  return OEMU_OK;
}

static oemu_status decode_cond_select(uint32_t word, oemu_insn *insn) {
  /* Only bit 11 is reserved here. Bit 10 is `o2`, which together with bit 30
   * selects CSEL/CSINC/CSINV/CSNEG -- treating it as reserved rejects half the
   * family, including the very common CSET and CINC aliases. */
  if (BIT(word, 29) != 0U || BIT(word, 11) != 0U) {
    return OEMU_ERR_DECODE;
  }
  const uint32_t op = BIT(word, 30);
  const uint32_t o2 = BIT(word, 10);

  insn->width = width_from_sf(word);
  insn->rd = field_rd(word);
  insn->rn = field_rn(word);
  insn->rm = field_rm(word);
  insn->cond = to_cond(BITS(word, 12, 4));
  insn->operand_kind = OEMU_OPERAND_REG;

  if (op == 0U) {
    insn->op = (o2 == 0U) ? OEMU_OP_CSEL : OEMU_OP_CSINC;
  } else {
    insn->op = (o2 == 0U) ? OEMU_OP_CSINV : OEMU_OP_CSNEG;
  }
  return OEMU_OK;
}

static oemu_status decode_dp_1source(uint32_t word, oemu_insn *insn) {
  if (BIT(word, 29) != 0U || BITS(word, 16, 5) != 0U) {
    return OEMU_ERR_DECODE;
  }
  const uint32_t opcode = BITS(word, 10, 6);
  const oemu_reg_width width = width_from_sf(word);

  insn->width = width;
  insn->rd = field_rd(word);
  insn->rn = field_rn(word);
  insn->operand_kind = OEMU_OPERAND_NONE;

  switch (opcode) {
    case 0x0:
      insn->op = OEMU_OP_RBIT;
      return OEMU_OK;
    case 0x1:
      insn->op = OEMU_OP_REV16;
      return OEMU_OK;
    case 0x2:
      /* At 32-bit width this encoding is REV; at 64-bit it is REV32. */
      insn->op = (width == OEMU_REG_W32) ? OEMU_OP_REV : OEMU_OP_REV32;
      return OEMU_OK;
    case 0x3:
      if (width == OEMU_REG_W32) {
        return OEMU_ERR_DECODE; /* REV64 does not exist at 32 bits */
      }
      insn->op = OEMU_OP_REV;
      return OEMU_OK;
    case 0x4:
      insn->op = OEMU_OP_CLZ;
      return OEMU_OK;
    case 0x5:
      insn->op = OEMU_OP_CLS;
      return OEMU_OK;
    default:
      return OEMU_ERR_DECODE;
  }
}

static oemu_status decode_dp_2source(uint32_t word, oemu_insn *insn) {
  if (BIT(word, 29) != 0U) {
    return OEMU_ERR_DECODE;
  }
  const uint32_t opcode = BITS(word, 10, 6);

  insn->width = width_from_sf(word);
  insn->rd = field_rd(word);
  insn->rn = field_rn(word);
  insn->rm = field_rm(word);
  insn->operand_kind = OEMU_OPERAND_REG;

  switch (opcode) {
    case 0x02:
      insn->op = OEMU_OP_UDIV;
      return OEMU_OK;
    case 0x03:
      insn->op = OEMU_OP_SDIV;
      return OEMU_OK;
    case 0x08:
      insn->op = OEMU_OP_LSLV;
      return OEMU_OK;
    case 0x09:
      insn->op = OEMU_OP_LSRV;
      return OEMU_OK;
    case 0x0A:
      insn->op = OEMU_OP_ASRV;
      return OEMU_OK;
    case 0x0B:
      insn->op = OEMU_OP_RORV;
      return OEMU_OK;
    default:
      /* CRC32 and the pointer-authentication variants live here. */
      if (opcode >= 0x10U && opcode <= 0x17U) {
        return OEMU_ERR_UNSUPPORTED;
      }
      return OEMU_ERR_DECODE;
  }
}

static oemu_status decode_dp_3source(uint32_t word, oemu_insn *insn) {
  if (BITS(word, 29, 2) != 0U) {
    return OEMU_ERR_DECODE;
  }
  const uint32_t op31 = BITS(word, 21, 3);
  const uint32_t o0 = BIT(word, 15);
  const oemu_reg_width width = width_from_sf(word);

  insn->width = width;
  insn->rd = field_rd(word);
  insn->rn = field_rn(word);
  insn->rm = field_rm(word);
  insn->ra = field_ra(word);
  insn->operand_kind = OEMU_OPERAND_REG;

  if (op31 == 0x0U) {
    insn->op = (o0 == 0U) ? OEMU_OP_MADD : OEMU_OP_MSUB;
    return OEMU_OK;
  }

  /* The widening and high-multiply forms exist only at 64-bit width. */
  if (width != OEMU_REG_W64) {
    return OEMU_ERR_DECODE;
  }
  switch (op31) {
    case 0x1:
      insn->op = (o0 == 0U) ? OEMU_OP_SMADDL : OEMU_OP_SMSUBL;
      return OEMU_OK;
    case 0x2:
      if (o0 != 0U) {
        return OEMU_ERR_DECODE;
      }
      insn->op = OEMU_OP_SMULH;
      return OEMU_OK;
    case 0x5:
      insn->op = (o0 == 0U) ? OEMU_OP_UMADDL : OEMU_OP_UMSUBL;
      return OEMU_OK;
    case 0x6:
      if (o0 != 0U) {
        return OEMU_ERR_DECODE;
      }
      insn->op = OEMU_OP_UMULH;
      return OEMU_OK;
    default:
      return OEMU_ERR_DECODE;
  }
}

static oemu_status decode_dp_register(uint32_t word, oemu_insn *insn) {
  const uint32_t op1 = BIT(word, 28);
  const uint32_t op2 = BITS(word, 21, 4);

  if (op1 == 0U) {
    /* Logical shifted register, or add/sub shifted/extended. */
    if ((op2 & 0x8U) == 0U) {
      return decode_logical_shifted(word, insn);
    }
    if ((op2 & 0x1U) != 0U) {
      return decode_addsub_extended(word, insn);
    }
    return decode_addsub_shifted(word, insn);
  }

  switch (op2) {
    case 0x0:
      return decode_addsub_carry(word, insn);
    case 0x2:
      return decode_cond_compare(word, insn);
    case 0x4:
      return decode_cond_select(word, insn);
    case 0x6:
      return (BIT(word, 30) != 0U) ? decode_dp_1source(word, insn)
                                   : decode_dp_2source(word, insn);
    default:
      if ((op2 & 0x8U) != 0U) {
        return decode_dp_3source(word, insn);
      }
      return OEMU_ERR_DECODE;
  }
}

/* --- loads and stores (op0 == x1x0) ----------------------------------------- */

/*
 * Maps the two-bit `opc` of the immediate load/store forms onto an operation and
 * a signedness, given the access size. The awkward case is opc == 3, which means
 * "load signed 32-bit" for the byte/half sizes but is unallocated at dword.
 */
static oemu_status classify_load_store(uint32_t opc, oemu_mem_size size, oemu_insn *insn) {
  if (opc == 0x0U) {
    insn->op = OEMU_OP_STR;
    return OEMU_OK;
  }
  if (opc == 0x1U) {
    insn->op = OEMU_OP_LDR;
    return OEMU_OK;
  }
  /* opc 2 and 3 are the sign-extending loads. */
  if (size == OEMU_MEM_DWORD) {
    /* No sign-extending 64-bit load exists; opc 2 at dword is PRFM. */
    return (opc == 0x2U) ? OEMU_ERR_UNSUPPORTED : OEMU_ERR_DECODE;
  }
  if (size == OEMU_MEM_WORD && opc == 0x3U) {
    return OEMU_ERR_DECODE; /* LDRSW to a 32-bit register is not encodable */
  }
  insn->op = OEMU_OP_LDRS;
  insn->is_signed_load = true;
  /* opc == 2 targets a 64-bit register, opc == 3 a 32-bit one. */
  insn->width = (opc == 0x2U) ? OEMU_REG_W64 : OEMU_REG_W32;
  return OEMU_OK;
}

static oemu_status decode_ldst_unsigned_imm(uint32_t word, oemu_insn *insn) {
  const oemu_mem_size size = to_mem_size(BITS(word, 30, 2));
  const uint32_t opc = BITS(word, 22, 2);

  insn->mem_size = size;
  insn->width = (size == OEMU_MEM_DWORD) ? OEMU_REG_W64 : OEMU_REG_W32;
  const oemu_status status = classify_load_store(opc, size, insn);
  if (status != OEMU_OK) {
    return status;
  }

  insn->rd = field_rd(word);
  insn->rn = field_rn(word);
  insn->rt2 = field_rd(word);
  insn->operand_kind = OEMU_OPERAND_MEM;
  insn->index_mode = OEMU_INDEX_NONE;
  /* The 12-bit immediate is unsigned and scaled by the access size. */
  insn->uimm = (uint64_t)BITS(word, 10, 12) << (unsigned)size;
  insn->imm = (int64_t)insn->uimm;
  insn->rn_is_sp_form = true;
  return OEMU_OK;
}

static oemu_status decode_ldst_imm_unscaled(uint32_t word, oemu_insn *insn) {
  const oemu_mem_size size = to_mem_size(BITS(word, 30, 2));
  const uint32_t opc = BITS(word, 22, 2);
  const uint32_t form = BITS(word, 10, 2);

  insn->mem_size = size;
  insn->width = (size == OEMU_MEM_DWORD) ? OEMU_REG_W64 : OEMU_REG_W32;
  const oemu_status status = classify_load_store(opc, size, insn);
  if (status != OEMU_OK) {
    return status;
  }

  switch (form) {
    case 0x0:
      insn->index_mode = OEMU_INDEX_NONE; /* STUR/LDUR */
      break;
    case 0x1:
      insn->index_mode = OEMU_INDEX_POST;
      break;
    case 0x3:
      insn->index_mode = OEMU_INDEX_PRE;
      break;
    default:
      /* form == 2 is the unprivileged LDTR/STTR family. */
      return OEMU_ERR_UNSUPPORTED;
  }

  insn->rd = field_rd(word);
  insn->rn = field_rn(word);
  insn->rt2 = field_rd(word);
  insn->operand_kind = OEMU_OPERAND_MEM;
  /* The 9-bit immediate is signed and never scaled. */
  insn->imm = oemu_decode_internal_sign_extend(BITS(word, 12, 9), 9U);
  insn->uimm = (uint64_t)insn->imm;
  insn->rn_is_sp_form = true;
  return OEMU_OK;
}

static oemu_status decode_ldst_reg_offset(uint32_t word, oemu_insn *insn) {
  const oemu_mem_size size = to_mem_size(BITS(word, 30, 2));
  const uint32_t opc = BITS(word, 22, 2);
  const uint32_t option = BITS(word, 13, 3);

  /* option == 0 or 4 would mean an 8-bit index register, which is not encodable. */
  if ((option & 0x2U) == 0U) {
    return OEMU_ERR_DECODE;
  }

  insn->mem_size = size;
  insn->width = (size == OEMU_MEM_DWORD) ? OEMU_REG_W64 : OEMU_REG_W32;
  const oemu_status status = classify_load_store(opc, size, insn);
  if (status != OEMU_OK) {
    return status;
  }

  insn->rd = field_rd(word);
  insn->rn = field_rn(word);
  insn->rm = field_rm(word);
  insn->rt2 = field_rd(word);
  insn->operand_kind = OEMU_OPERAND_MEM;
  insn->index_mode = OEMU_INDEX_NONE;
  insn->extend_type = to_extend_type(option);
  /* S selects whether the index is scaled by the access size. */
  insn->shift_amount = (BIT(word, 12) != 0U) ? (unsigned)size : 0U;
  insn->extend_is_lsl = (option == 0x3U); /* UXTX with a 64-bit index is an LSL */
  insn->rn_is_sp_form = true;
  return OEMU_OK;
}

static oemu_status decode_ldst_pair(uint32_t word, oemu_insn *insn) {
  const uint32_t opc = BITS(word, 30, 2);
  const bool is_load = BIT(word, 22) != 0U;
  const uint32_t form = BITS(word, 23, 2);

  if (opc == 0x3U) {
    return OEMU_ERR_DECODE;
  }
  if (opc == 0x1U) {
    /* opc 1 is LDPSW when loading, and unallocated when storing. */
    if (!is_load) {
      return OEMU_ERR_DECODE;
    }
    insn->op = OEMU_OP_LDPSW;
    insn->mem_size = OEMU_MEM_WORD;
    insn->width = OEMU_REG_W64;
    insn->is_signed_load = true;
  } else {
    const bool is_64 = (opc == 0x2U);
    insn->op = is_load ? OEMU_OP_LDP : OEMU_OP_STP;
    insn->mem_size = is_64 ? OEMU_MEM_DWORD : OEMU_MEM_WORD;
    insn->width = is_64 ? OEMU_REG_W64 : OEMU_REG_W32;
  }

  switch (form) {
    case 0x1:
      insn->index_mode = OEMU_INDEX_POST;
      break;
    case 0x2:
      insn->index_mode = OEMU_INDEX_NONE;
      break;
    case 0x3:
      insn->index_mode = OEMU_INDEX_PRE;
      break;
    default:
      /* form == 0 is the non-temporal LDNP/STNP pair. */
      return OEMU_ERR_UNSUPPORTED;
  }

  insn->rd = field_rd(word);
  insn->rn = field_rn(word);
  insn->rt2 = BITS(word, 10, 5);
  insn->operand_kind = OEMU_OPERAND_MEM;
  /* The 7-bit immediate is signed and scaled by the transfer size. */
  insn->imm = oemu_decode_internal_sign_extend(BITS(word, 15, 7), 7U) *
              (int64_t)(UINT64_C(1) << (unsigned)insn->mem_size);
  insn->uimm = (uint64_t)insn->imm;
  insn->rn_is_sp_form = true;
  return OEMU_OK;
}

static oemu_status decode_ldst_literal(uint32_t word, uint64_t pc, oemu_insn *insn) {
  const uint32_t opc = BITS(word, 30, 2);
  if (opc == 0x3U) {
    return OEMU_ERR_UNSUPPORTED; /* PRFM literal */
  }
  if (opc == 0x2U) {
    insn->op = OEMU_OP_LDRS; /* LDRSW literal */
    insn->is_signed_load = true;
    insn->mem_size = OEMU_MEM_WORD;
    insn->width = OEMU_REG_W64;
  } else {
    insn->op = OEMU_OP_LDR;
    insn->mem_size = (opc == 0x1U) ? OEMU_MEM_DWORD : OEMU_MEM_WORD;
    insn->width = (opc == 0x1U) ? OEMU_REG_W64 : OEMU_REG_W32;
  }

  insn->rd = field_rd(word);
  insn->rt2 = field_rd(word);
  insn->operand_kind = OEMU_OPERAND_IMM;
  /* The address is PC-relative, scaled by 4, and already resolved here. */
  const int64_t offset = oemu_decode_internal_sign_extend(BITS(word, 5, 19), 19U);
  insn->imm = (int64_t)(pc + ((uint64_t)offset << 2U));
  return OEMU_OK;
}

static oemu_status decode_ldst_exclusive(uint32_t word, oemu_insn *insn) {
  const oemu_mem_size size = to_mem_size(BITS(word, 30, 2));
  const uint32_t o2 = BIT(word, 23);
  const uint32_t l = BIT(word, 22);
  const uint32_t o1 = BIT(word, 21);
  const uint32_t o0 = BIT(word, 15);

  if (o1 != 0U) {
    /* The pair variants LDXP/STXP need two transfer registers. */
    return OEMU_ERR_UNSUPPORTED;
  }

  insn->mem_size = size;
  insn->width = (size == OEMU_MEM_DWORD) ? OEMU_REG_W64 : OEMU_REG_W32;
  insn->rd = field_rd(word);
  insn->rn = field_rn(word);
  insn->rm = BITS(word, 16, 5); /* Rs: where STXR reports success */
  insn->rt2 = field_rd(word);
  insn->operand_kind = OEMU_OPERAND_MEM;
  insn->rn_is_sp_form = true;

  if (o2 == 0U) {
    /* Load/store exclusive; o0 selects the acquire/release ordering. */
    insn->op = (l != 0U) ? OEMU_OP_LDXR : OEMU_OP_STXR;
    return OEMU_OK;
  }
  /* Load-acquire / store-release, which carry no reservation. */
  if (o0 == 0U) {
    return OEMU_ERR_DECODE;
  }
  insn->op = (l != 0U) ? OEMU_OP_LDAR : OEMU_OP_STLR;
  return OEMU_OK;
}

static oemu_status decode_load_store(uint32_t word, uint64_t pc, oemu_insn *insn) {
  /* Bit 31 clear together with the SIMD selector marks the vector forms. */
  if (BIT(word, 26) != 0U) {
    return OEMU_ERR_UNSUPPORTED; /* SIMD load/store */
  }

  const uint32_t op0 = BITS(word, 28, 2);

  if (op0 == 0x0U) {
    /* Exclusive and ordered accesses. */
    if (BITS(word, 24, 2) != 0U) {
      return OEMU_ERR_DECODE;
    }
    return decode_ldst_exclusive(word, insn);
  }

  if (op0 == 0x1U) {
    /* Literal pool loads. */
    if (BIT(word, 24) != 0U) {
      return OEMU_ERR_DECODE;
    }
    return decode_ldst_literal(word, pc, insn);
  }

  if (op0 == 0x2U) {
    return decode_ldst_pair(word, insn);
  }

  /* op0 == 3: the register and immediate addressed forms. */
  if (BIT(word, 24) != 0U) {
    return decode_ldst_unsigned_imm(word, insn);
  }
  if (BIT(word, 21) != 0U) {
    return decode_ldst_reg_offset(word, insn);
  }
  return decode_ldst_imm_unscaled(word, insn);
}

/* --- top level --------------------------------------------------------------- */

oemu_status oemu_decode(uint32_t word, uint64_t pc, oemu_insn *out) {
  if (out == NULL) {
    return OEMU_ERR_INVALID_ARG;
  }

  insn_reset(out, word);

  const uint32_t op0 = BITS(word, 25, 4);
  oemu_status status = OEMU_ERR_DECODE;

  switch (op0) {
    case 0x8: /* 100x */
    case 0x9:
      status = decode_dp_immediate(word, pc, out);
      break;
    case 0xA: /* 101x */
    case 0xB:
      status = decode_branch_system(word, pc, out);
      break;
    case 0x4: /* x1x0 */
    case 0x6:
    case 0xC:
    case 0xE:
      status = decode_load_store(word, pc, out);
      break;
    case 0x5: /* x101 */
    case 0xD:
      status = decode_dp_register(word, out);
      break;
    case 0x7: /* x111: SIMD and floating point */
    case 0xF:
      status = OEMU_ERR_UNSUPPORTED;
      break;
    default:
      /* op0 == 000x is unallocated; 0010/0011 is the SVE space. */
      status = (op0 == 0x2U || op0 == 0x3U) ? OEMU_ERR_UNSUPPORTED : OEMU_ERR_DECODE;
      break;
  }

  if (status != OEMU_OK) {
    /* Leave nothing partially decoded: a caller that ignores the status must not
     * find fields that look plausible. */
    insn_reset(out, word);
  }
  return status;
}

const char *oemu_opcode_name(oemu_opcode op) {
  switch (op) {
    case OEMU_OP_UNKNOWN:
      return "unknown";
    case OEMU_OP_ADD:
      return "add";
    case OEMU_OP_ADDS:
      return "adds";
    case OEMU_OP_SUB:
      return "sub";
    case OEMU_OP_SUBS:
      return "subs";
    case OEMU_OP_ADC:
      return "adc";
    case OEMU_OP_ADCS:
      return "adcs";
    case OEMU_OP_SBC:
      return "sbc";
    case OEMU_OP_SBCS:
      return "sbcs";
    case OEMU_OP_AND:
      return "and";
    case OEMU_OP_ANDS:
      return "ands";
    case OEMU_OP_ORR:
      return "orr";
    case OEMU_OP_EOR:
      return "eor";
    case OEMU_OP_BIC:
      return "bic";
    case OEMU_OP_BICS:
      return "bics";
    case OEMU_OP_ORN:
      return "orn";
    case OEMU_OP_EON:
      return "eon";
    case OEMU_OP_MOVZ:
      return "movz";
    case OEMU_OP_MOVN:
      return "movn";
    case OEMU_OP_MOVK:
      return "movk";
    case OEMU_OP_ADR:
      return "adr";
    case OEMU_OP_ADRP:
      return "adrp";
    case OEMU_OP_SBFM:
      return "sbfm";
    case OEMU_OP_BFM:
      return "bfm";
    case OEMU_OP_UBFM:
      return "ubfm";
    case OEMU_OP_EXTR:
      return "extr";
    case OEMU_OP_LSLV:
      return "lslv";
    case OEMU_OP_LSRV:
      return "lsrv";
    case OEMU_OP_ASRV:
      return "asrv";
    case OEMU_OP_RORV:
      return "rorv";
    case OEMU_OP_RBIT:
      return "rbit";
    case OEMU_OP_REV16:
      return "rev16";
    case OEMU_OP_REV32:
      return "rev32";
    case OEMU_OP_REV:
      return "rev";
    case OEMU_OP_CLZ:
      return "clz";
    case OEMU_OP_CLS:
      return "cls";
    case OEMU_OP_UDIV:
      return "udiv";
    case OEMU_OP_SDIV:
      return "sdiv";
    case OEMU_OP_MADD:
      return "madd";
    case OEMU_OP_MSUB:
      return "msub";
    case OEMU_OP_SMADDL:
      return "smaddl";
    case OEMU_OP_SMSUBL:
      return "smsubl";
    case OEMU_OP_UMADDL:
      return "umaddl";
    case OEMU_OP_UMSUBL:
      return "umsubl";
    case OEMU_OP_SMULH:
      return "smulh";
    case OEMU_OP_UMULH:
      return "umulh";
    case OEMU_OP_CSEL:
      return "csel";
    case OEMU_OP_CSINC:
      return "csinc";
    case OEMU_OP_CSINV:
      return "csinv";
    case OEMU_OP_CSNEG:
      return "csneg";
    case OEMU_OP_CCMP:
      return "ccmp";
    case OEMU_OP_CCMN:
      return "ccmn";
    case OEMU_OP_B:
      return "b";
    case OEMU_OP_BL:
      return "bl";
    case OEMU_OP_B_COND:
      return "b.cond";
    case OEMU_OP_BR:
      return "br";
    case OEMU_OP_BLR:
      return "blr";
    case OEMU_OP_RET:
      return "ret";
    case OEMU_OP_CBZ:
      return "cbz";
    case OEMU_OP_CBNZ:
      return "cbnz";
    case OEMU_OP_TBZ:
      return "tbz";
    case OEMU_OP_TBNZ:
      return "tbnz";
    case OEMU_OP_LDR:
      return "ldr";
    case OEMU_OP_STR:
      return "str";
    case OEMU_OP_LDRS:
      return "ldrs";
    case OEMU_OP_LDP:
      return "ldp";
    case OEMU_OP_STP:
      return "stp";
    case OEMU_OP_LDPSW:
      return "ldpsw";
    case OEMU_OP_LDXR:
      return "ldxr";
    case OEMU_OP_STXR:
      return "stxr";
    case OEMU_OP_LDAR:
      return "ldar";
    case OEMU_OP_STLR:
      return "stlr";
    case OEMU_OP_SVC:
      return "svc";
    case OEMU_OP_BRK:
      return "brk";
    case OEMU_OP_HLT:
      return "hlt";
    case OEMU_OP_NOP:
      return "nop";
    case OEMU_OP_HINT:
      return "hint";
    case OEMU_OP_BARRIER:
      return "barrier";
    case OEMU_OP_MRS:
      return "mrs";
    case OEMU_OP_MSR:
      return "msr";
    default:
      break;
  }
  return "unknown";
}
