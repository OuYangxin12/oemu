/*
 * ARMv8-A AArch64 user-mode (EL0) architectural register state.
 *
 * Models exactly what the emulated subset needs: the 31 general-purpose
 * registers, SP, PC and the NZCV condition flags. No FP/SIMD state, no system
 * registers, no banked per-exception-level SP -- EL0 sees only SP_EL0.
 *
 * Two rules from the architecture drive the whole API and are easy to get
 * wrong, so they are encoded in the function set rather than left to callers:
 *
 *   1. Register number 31 is not a register. Depending on the instruction
 *      encoding it means either the zero register (XZR/WZR) or the stack
 *      pointer. Nothing about the number itself distinguishes the two, so this
 *      header offers two families of accessors and the decoder picks per
 *      instruction. See oemu_regs_read / oemu_regs_read_sp_form.
 *
 *   2. A 32-bit (W) write zero-extends into the full 64-bit register. It does
 *      not preserve the upper half. Every write path here enforces that.
 *
 * The struct is not opaque, so it can live on the stack; the module performs no
 * allocation at all.
 */
#ifndef OEMU_REGS_H
#define OEMU_REGS_H

#include "oemu/macros.h"
#include "oemu/status.h"

#include <stdbool.h>
#include <stdint.h>

OEMU_BEGIN_DECLS

/* X0..X30 occupy slots; register number 31 never indexes this array. */
#define OEMU_REG_COUNT ((unsigned)31)

/*
 * The encoded register number that means XZR/WZR or SP. It is an encoding
 * value, never an index into oemu_regs.x.
 */
#define OEMU_REG_ZR ((unsigned)31)

/* A64 instructions are fixed width. */
#define OEMU_INSN_SIZE ((unsigned)4)

/* Operand width selector. The values are the bit counts, so they read clearly
 * at call sites and in test failure messages. */
typedef enum oemu_reg_width { OEMU_REG_W32 = 32, OEMU_REG_W64 = 64 } oemu_reg_width;

/*
 * Condition flags, packed at bits 31..28 exactly as in the NZCV system
 * register. Keeping the architectural layout means a future MRS/MSR of NZCV
 * needs no translation.
 */
#define OEMU_NZCV_N ((uint32_t)0x80000000U)
#define OEMU_NZCV_Z ((uint32_t)0x40000000U)
#define OEMU_NZCV_C ((uint32_t)0x20000000U)
#define OEMU_NZCV_V ((uint32_t)0x10000000U)

/* Mask of the bits oemu_regs stores; the low 28 bits are RES0 at EL0. */
#define OEMU_NZCV_MASK (OEMU_NZCV_N | OEMU_NZCV_Z | OEMU_NZCV_C | OEMU_NZCV_V)

/*
 * The 4-bit condition field. Enumerator values are the encodings themselves, so
 * a decoded field can be cast directly.
 *
 * Note the last pair: AL (0b1110) and NV (0b1111) both mean "always" in
 * AArch64. NV is not "never" -- that reading is a common and silent bug.
 */
typedef enum oemu_cond {
  OEMU_COND_EQ = 0x0, /* equal:                      Z == 1 */
  OEMU_COND_NE = 0x1, /* not equal:                  Z == 0 */
  OEMU_COND_CS = 0x2, /* carry set / unsigned >=:    C == 1 */
  OEMU_COND_CC = 0x3, /* carry clear / unsigned <:   C == 0 */
  OEMU_COND_MI = 0x4, /* minus:                      N == 1 */
  OEMU_COND_PL = 0x5, /* plus or zero:               N == 0 */
  OEMU_COND_VS = 0x6, /* overflow set:               V == 1 */
  OEMU_COND_VC = 0x7, /* overflow clear:             V == 0 */
  OEMU_COND_HI = 0x8, /* unsigned >:                 C == 1 && Z == 0 */
  OEMU_COND_LS = 0x9, /* unsigned <=:                C == 0 || Z == 1 */
  OEMU_COND_GE = 0xA, /* signed >=:                  N == V */
  OEMU_COND_LT = 0xB, /* signed <:                   N != V */
  OEMU_COND_GT = 0xC, /* signed >:                   Z == 0 && N == V */
  OEMU_COND_LE = 0xD, /* signed <=:                  Z == 1 || N != V */
  OEMU_COND_AL = 0xE, /* always */
  OEMU_COND_NV = 0xF  /* also always, despite the mnemonic */
} oemu_cond;

/*
 * Architectural register state. Treat the fields as read-only and use the
 * accessors: they implement the width and register-31 rules described above.
 */
typedef struct oemu_regs {
  uint64_t x[OEMU_REG_COUNT]; /* X0..X30 */
  uint64_t sp;                /* SP_EL0 */
  uint64_t pc;                /* address of the current instruction */
  uint32_t nzcv;              /* condition flags at bits 31..28 */
} oemu_regs;

/* Result of a flag-setting arithmetic operation. */
typedef struct oemu_alu_result {
  uint64_t value; /* already truncated to the operand width */
  uint32_t nzcv;  /* the four flags, positioned as in OEMU_NZCV_* */
} oemu_alu_result;

/*
 * Zeroes every register, then sets PC and SP. Returns OEMU_ERR_INVALID_ARG if
 * `regs` is NULL. This is the only function here that reports a status: the
 * accessors treat a bad argument as a programming error and abort, because a
 * register number comes from a 5-bit instruction field and therefore cannot be
 * out of range unless the caller is broken.
 */
OEMU_NODISCARD oemu_status oemu_regs_init(oemu_regs *regs, uint64_t entry_pc,
                                          uint64_t initial_sp);

/* --- general-purpose registers, zero-register form -------------------------- */
/*
 * The form used by most instructions: register number 31 reads as zero and
 * discards writes. Aborts if `n` > 31 or `width` is not a valid width.
 */

uint64_t oemu_regs_read(const oemu_regs *regs, unsigned n, oemu_reg_width width);

/* A W-width write zero-extends to 64 bits; it does not preserve the top half. */
void oemu_regs_write(oemu_regs *regs, unsigned n, oemu_reg_width width, uint64_t value);

/* --- general-purpose registers, stack-pointer form -------------------------- */
/*
 * The form used where 31 encodes SP instead of the zero register: ADD/SUB with
 * an immediate, and the load/store base register. Identical to the functions
 * above for n < 31.
 */

uint64_t oemu_regs_read_sp_form(const oemu_regs *regs, unsigned n, oemu_reg_width width);

void oemu_regs_write_sp_form(oemu_regs *regs, unsigned n, oemu_reg_width width, uint64_t value);

/* --- SP, PC and flags ------------------------------------------------------- */

uint64_t oemu_regs_sp(const oemu_regs *regs);
void oemu_regs_set_sp(oemu_regs *regs, uint64_t value);

/*
 * Reading PC yields the address of the instruction being executed. Unlike A32,
 * AArch64 has no read offset.
 */
uint64_t oemu_regs_pc(const oemu_regs *regs);
void oemu_regs_set_pc(oemu_regs *regs, uint64_t value);

/* Advances PC to the next instruction. Wraps at 2^64 rather than failing:
 * whether the resulting address is executable is the memory subsystem's call. */
void oemu_regs_advance_pc(oemu_regs *regs);

/* PC-relative branch. `byte_offset` is the already-decoded and scaled offset. */
void oemu_regs_branch_rel(oemu_regs *regs, int64_t byte_offset);

uint32_t oemu_regs_nzcv(const oemu_regs *regs);

/* Stores only the flag bits; anything outside OEMU_NZCV_MASK is dropped. */
void oemu_regs_set_nzcv(oemu_regs *regs, uint32_t nzcv);

/* Evaluates a condition against the current flags. */
bool oemu_regs_cond_holds(const oemu_regs *regs, oemu_cond cond);

/* --- flag derivation -------------------------------------------------------- */
/*
 * The architecture's AddWithCarry primitive, and the single source of NZCV for
 * every flag-setting arithmetic instruction. Pure function: no register state
 * involved.
 *
 *   ADDS         -> add_with_carry(x,  y, false, width)
 *   SUBS / CMP   -> add_with_carry(x, ~y, true,  width)
 *   ADCS         -> add_with_carry(x,  y, carry, width)
 *
 * Expressing subtraction this way is what makes C mean "no borrow" and keeps V
 * correct for both directions, so callers should not compute flags themselves.
 */
oemu_alu_result oemu_regs_add_with_carry(uint64_t x, uint64_t y, bool carry_in,
                                         oemu_reg_width width);

OEMU_END_DECLS

#endif /* OEMU_REGS_H */
