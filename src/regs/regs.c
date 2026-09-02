#include "oemu/regs.h"

#include "oemu/check.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "regs_internal.h"

/* --- shared contract checks -------------------------------------------------- */
/*
 * A register number comes from a 5-bit instruction field and a width from the
 * `sf` bit, so neither can be invalid unless the caller is broken. That is a
 * programming error rather than a recoverable condition, hence OEMU_REQUIRE.
 */

static void require_valid_reg(const oemu_regs *regs, unsigned n) {
  OEMU_REQUIRE(regs != NULL, "register access on a NULL oemu_regs");
  OEMU_REQUIRE(n <= OEMU_REG_ZR, "register number above 31");
}

static void require_valid_width(oemu_reg_width width) {
  OEMU_REQUIRE(width == OEMU_REG_W32 || width == OEMU_REG_W64,
               "operand width must be OEMU_REG_W32 or OEMU_REG_W64");
}

/* --- internal helpers (exposed via regs_internal.h for white-box tests) ------ */

uint64_t oemu_regs_internal_truncate(uint64_t value, oemu_reg_width width) {
  return (width == OEMU_REG_W32) ? (value & UINT64_C(0xFFFFFFFF)) : value;
}

bool oemu_regs_internal_cond_holds(uint32_t nzcv, oemu_cond cond) {
  const bool n = (nzcv & OEMU_NZCV_N) != 0U;
  const bool z = (nzcv & OEMU_NZCV_Z) != 0U;
  const bool c = (nzcv & OEMU_NZCV_C) != 0U;
  const bool v = (nzcv & OEMU_NZCV_V) != 0U;

  const unsigned encoding = (unsigned)cond & 0xFU;

  /*
   * The 16 conditions form 8 pairs: the top three bits select the base test and
   * the low bit requests its inverse. Decoding it that way, rather than as a
   * 16-arm switch, makes the complement property structural.
   */
  bool result = false;
  switch ((encoding >> 1) & 0x7U) {
    case 0x0: /* EQ / NE */
      result = z;
      break;
    case 0x1: /* CS / CC */
      result = c;
      break;
    case 0x2: /* MI / PL */
      result = n;
      break;
    case 0x3: /* VS / VC */
      result = v;
      break;
    case 0x4: /* HI / LS */
      result = c && !z;
      break;
    case 0x5: /* GE / LT */
      result = (n == v);
      break;
    case 0x6: /* GT / LE */
      result = (n == v) && !z;
      break;
    default: /* 0x7: AL / NV */
      result = true;
      break;
  }

  /*
   * The low bit inverts the base test -- except for 0b1111. AL and NV both mean
   * "always" in AArch64; reading NV as "never" is a silent misexecution bug, so
   * the exception is explicit here.
   */
  if ((encoding & 0x1U) != 0U && encoding != 0xFU) {
    result = !result;
  }
  return result;
}

oemu_alu_result oemu_regs_internal_add_with_carry(uint64_t x, uint64_t y, bool carry_in,
                                                  oemu_reg_width width) {
  const uint64_t carry = carry_in ? UINT64_C(1) : UINT64_C(0);
  const uint64_t xa = oemu_regs_internal_truncate(x, width);
  const uint64_t ya = oemu_regs_internal_truncate(y, width);

  const uint64_t sign =
      (width == OEMU_REG_W32) ? UINT64_C(0x80000000) : UINT64_C(0x8000000000000000);

  const uint64_t sum = oemu_regs_internal_truncate(xa + ya + carry, width);

  bool carry_out = false;
  if (width == OEMU_REG_W32) {
    /* Both operands fit in 32 bits, so the untruncated sum cannot overflow a
     * uint64_t and bit 32 is the carry directly. */
    carry_out = ((xa + ya + carry) >> 32U) != 0U;
  } else {
    /* At 64 bits the addition itself can wrap, so detect each step. */
    const uint64_t partial = xa + ya;
    carry_out = (partial < xa) || (carry_in && partial == UINT64_MAX);
  }

  const bool x_neg = (xa & sign) != 0U;
  const bool y_neg = (ya & sign) != 0U;
  const bool sum_neg = (sum & sign) != 0U;

  uint32_t flags = 0U;
  if (sum_neg) {
    flags |= OEMU_NZCV_N;
  }
  if (sum == 0U) {
    flags |= OEMU_NZCV_Z;
  }
  if (carry_out) {
    flags |= OEMU_NZCV_C;
  }
  /* Signed overflow: operands agreed in sign and the result disagrees. */
  if (x_neg == y_neg && sum_neg != x_neg) {
    flags |= OEMU_NZCV_V;
  }

  oemu_alu_result result;
  result.value = sum;
  result.nzcv = flags;
  return result;
}

/* --- initialisation ---------------------------------------------------------- */

oemu_status oemu_regs_init(oemu_regs *regs, uint64_t entry_pc, uint64_t initial_sp) {
  if (regs == NULL) {
    return OEMU_ERR_INVALID_ARG;
  }

  memset(regs, 0, sizeof(*regs));
  regs->pc = entry_pc;
  regs->sp = initial_sp;
  return OEMU_OK;
}

/* --- general-purpose registers, zero-register form --------------------------- */

uint64_t oemu_regs_read(const oemu_regs *regs, unsigned n, oemu_reg_width width) {
  require_valid_reg(regs, n);
  require_valid_width(width);

  if (n == OEMU_REG_ZR) {
    return 0U;
  }
  return oemu_regs_internal_truncate(regs->x[n], width);
}

void oemu_regs_write(oemu_regs *regs, unsigned n, oemu_reg_width width, uint64_t value) {
  require_valid_reg(regs, n);
  require_valid_width(width);

  /* Writes to the zero register are discarded -- and must not touch SP, which
   * shares the encoding but not this accessor. */
  if (n == OEMU_REG_ZR) {
    return;
  }
  regs->x[n] = oemu_regs_internal_truncate(value, width);
}

/* --- general-purpose registers, stack-pointer form --------------------------- */

uint64_t oemu_regs_read_sp_form(const oemu_regs *regs, unsigned n, oemu_reg_width width) {
  require_valid_reg(regs, n);
  require_valid_width(width);

  const uint64_t value = (n == OEMU_REG_ZR) ? regs->sp : regs->x[n];
  return oemu_regs_internal_truncate(value, width);
}

void oemu_regs_write_sp_form(oemu_regs *regs, unsigned n, oemu_reg_width width,
                             uint64_t value) {
  require_valid_reg(regs, n);
  require_valid_width(width);

  const uint64_t narrowed = oemu_regs_internal_truncate(value, width);
  if (n == OEMU_REG_ZR) {
    regs->sp = narrowed;
    return;
  }
  regs->x[n] = narrowed;
}

/* --- SP, PC and flags -------------------------------------------------------- */

uint64_t oemu_regs_sp(const oemu_regs *regs) {
  OEMU_REQUIRE(regs != NULL, "oemu_regs_sp on a NULL oemu_regs");
  return regs->sp;
}

void oemu_regs_set_sp(oemu_regs *regs, uint64_t value) {
  OEMU_REQUIRE(regs != NULL, "oemu_regs_set_sp on a NULL oemu_regs");
  regs->sp = value;
}

uint64_t oemu_regs_pc(const oemu_regs *regs) {
  OEMU_REQUIRE(regs != NULL, "oemu_regs_pc on a NULL oemu_regs");
  return regs->pc;
}

void oemu_regs_set_pc(oemu_regs *regs, uint64_t value) {
  OEMU_REQUIRE(regs != NULL, "oemu_regs_set_pc on a NULL oemu_regs");
  regs->pc = value;
}

void oemu_regs_advance_pc(oemu_regs *regs) {
  OEMU_REQUIRE(regs != NULL, "oemu_regs_advance_pc on a NULL oemu_regs");
  /* Unsigned wrap-around is well defined and is the architectural behaviour;
   * judging whether the new address is executable belongs to memory. */
  regs->pc += OEMU_INSN_SIZE;
}

void oemu_regs_branch_rel(oemu_regs *regs, int64_t byte_offset) {
  OEMU_REQUIRE(regs != NULL, "oemu_regs_branch_rel on a NULL oemu_regs");
  /* Convert through uint64_t so a negative offset wraps instead of overflowing
   * signed arithmetic, which would be undefined behaviour. */
  regs->pc += (uint64_t)byte_offset;
}

uint32_t oemu_regs_nzcv(const oemu_regs *regs) {
  OEMU_REQUIRE(regs != NULL, "oemu_regs_nzcv on a NULL oemu_regs");
  return regs->nzcv;
}

void oemu_regs_set_nzcv(oemu_regs *regs, uint32_t nzcv) {
  OEMU_REQUIRE(regs != NULL, "oemu_regs_set_nzcv on a NULL oemu_regs");
  /* The low 28 bits are RES0 at EL0; dropping them keeps the stored state
   * canonical so comparisons in tests and future MRS reads stay meaningful. */
  regs->nzcv = nzcv & OEMU_NZCV_MASK;
}

bool oemu_regs_cond_holds(const oemu_regs *regs, oemu_cond cond) {
  OEMU_REQUIRE(regs != NULL, "oemu_regs_cond_holds on a NULL oemu_regs");
  return oemu_regs_internal_cond_holds(regs->nzcv, cond);
}

/* --- flag derivation --------------------------------------------------------- */

oemu_alu_result oemu_regs_add_with_carry(uint64_t x, uint64_t y, bool carry_in,
                                         oemu_reg_width width) {
  require_valid_width(width);
  return oemu_regs_internal_add_with_carry(x, y, carry_in, width);
}
