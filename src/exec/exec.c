/*
 * A64 interpreter core. See include/oemu/exec.h for the contract and
 * src/exec/exec_internal.h for what each helper means.
 *
 * Every memory reference goes through the oemu_memops seam, never at a
 * concrete bus type: the same interpreter drives a flat test memory, a
 * physical address space, or (from M2 on) a machine's own bus.
 *
 * The dispatch order discipline that keeps faults precise:
 *
 *   1. read all sources into locals;
 *   2. validate/perform memory accesses into locals (never committing
 *      registers on the way);
 *   3. commit: result register, then writeback base, then PC.
 *
 * Because the decoder hands every immediate already final (scaled,
 * sign-extended, PC resolved), nothing here touches encoding bits, and an
 * instruction whose address computation faults has committed nothing.
 */
#include "oemu/check.h"
#include "oemu/exc.h"
#include "oemu/sysreg.h"

#include "exec_internal.h"

/* --- sysreg encoding constants ----------------------------------------------- */

/*
 * insn->sysreg keeps raw encoding bits 19:5. Bit 19 is op0<0>, which the MRS
 * versus MSR direction already fixes, so whitelisting masks it out and each
 * register has one number in both directions. The values were extracted from
 * the host assembler's own output for each named register, not from memory --
 * an off-by-one in these constants silently turns an MRS into a refusal.
 */
#define SYSREG_MASK        ((uint32_t)0x3FFFU)
#define SYSREG_CURRENT_EL  ((uint32_t)0x212U)
#define SYSREG_SP_EL0      ((uint32_t)0x208U)
#define SYSREG_NZCV        ((uint32_t)0x1A10U)
#define SYSREG_TPIDRRO_EL0 ((uint32_t)0x1E83U)
#define SYSREG_TPIDRUR_EL0 ((uint32_t)0x1E87U)

/* --- register access shorthands ----------------------------------------------- */

static uint64_t read_g(const oemu_cpu *cpu, unsigned n, bool sp_form, oemu_reg_width width) {
  return sp_form ? oemu_regs_read_sp_form(&cpu->regs, n, width)
                 : oemu_regs_read(&cpu->regs, n, width);
}

static oemu_status do_pair_vector(oemu_cpu *cpu, const oemu_memops *mem, const oemu_insn *in);

static void write_g(oemu_cpu *cpu, unsigned n, bool sp_form, oemu_reg_width width,
                    uint64_t value) {
  if (sp_form) {
    oemu_regs_write_sp_form(&cpu->regs, n, width, value);
  } else {
    oemu_regs_write(&cpu->regs, n, width, value);
  }
}

/*
 * After the bus's validate succeeded, an access with the same arguments
 * cannot fail -- no provider allocates or re-shapes between calls. A failure
 * here is therefore a bug in oemu, not a guest event, and is reported as one.
 */
static void access_or_panic(oemu_status st) {
  OEMU_REQUIRE(st == OEMU_OK, "validated memory access failed");
}

static unsigned reg_bits_of(oemu_reg_width width) {
  return (width == OEMU_REG_W32) ? 32U : 64U;
}

/* --- pure helpers (public through exec_internal.h) ---------------------------- */

/* Rotate right within `bits` (32 or 64), `amount` in 1..bits-1. */
static uint64_t rotr(uint64_t value, unsigned amount, unsigned bits) {
  const uint64_t mask = (bits == 64U) ? ~UINT64_C(0) : ((UINT64_C(1) << bits) - UINT64_C(1));
  return ((value >> amount) | (value << (bits - amount))) & mask;
}

oemu_exec_shift_result oemu_exec_internal_shift_operand(uint64_t value, oemu_shift_type type,
                                                        unsigned amount, oemu_reg_width width) {
  const unsigned bits = reg_bits_of(width);
  const uint64_t mask = (bits == 64U) ? ~UINT64_C(0) : ((UINT64_C(1) << bits) - UINT64_C(1));
  const uint64_t masked = value & mask;
  const bool sign = (masked & (UINT64_C(1) << (bits - 1U))) != 0U;
  oemu_exec_shift_result out = {masked, true, false};

  if (amount == 0U) {
    /* The imm6 == 0 edges; see the internal header. Each differs. */
    switch (type) {
      case OEMU_SHIFT_LSL:
        out.carry_valid = false; /* C keeps its old value */
        return out;
      case OEMU_SHIFT_LSR:
        out.value = 0U;
        out.carry = (masked >> (bits - 1U)) != 0U;
        return out;
      case OEMU_SHIFT_ASR:
      case OEMU_SHIFT_ROR:
        out.value = sign ? mask : 0U;
        out.carry = sign;
        return out;
    }
  }

  switch (type) {
    case OEMU_SHIFT_LSL:
      out.value = (masked << amount) & mask;
      break;
    case OEMU_SHIFT_LSR:
      out.value = masked >> amount;
      break;
    case OEMU_SHIFT_ASR: {
      /* Arithmetic shift at the operand width, not always 64; the fill must
       * not spill above bit `bits - 1` at 32-bit width. */
      const uint64_t fill = sign ? ((mask << (bits - amount)) & mask) : 0U;
      out.value = (((masked & mask) >> amount) | fill) & mask;
      break;
    }
    case OEMU_SHIFT_ROR:
      out.value = rotr(masked, amount, bits);
      break;
  }
  /* The carry is the last bit shifted out: the topmost bit for LSL, the
   * bottom-most reached bit for every right-rotating form. */
  const unsigned carry_bit = (type == OEMU_SHIFT_LSL) ? (bits - amount) : (amount - 1U);
  out.carry = ((masked >> carry_bit) & UINT64_C(1)) != 0U;
  return out;
}

uint64_t oemu_exec_internal_extend_operand(uint64_t index_value, oemu_extend_type type,
                                           unsigned shift, bool is_lsl) {
  uint64_t base;
  switch (type) {
    case OEMU_EXTEND_UXTB:
      base = index_value & UINT64_C(0xFF);
      break;
    case OEMU_EXTEND_UXTH:
      base = index_value & UINT64_C(0xFFFF);
      break;
    case OEMU_EXTEND_UXTW:
      base = index_value & UINT64_C(0xFFFFFFFF);
      break;
    case OEMU_EXTEND_SXTB:
      base = (uint64_t)(int64_t)(int8_t)(uint8_t)index_value;
      break;
    case OEMU_EXTEND_SXTH:
      base = (uint64_t)(int64_t)(int16_t)(uint16_t)index_value;
      break;
    case OEMU_EXTEND_SXTW:
      base = (uint64_t)(int64_t)(int32_t)(uint32_t)index_value;
      break;
    case OEMU_EXTEND_UXTX:
    case OEMU_EXTEND_SXTX:
    default:
      base = index_value;
      break;
  }
  (void)is_lsl; /* both branches shift; the distinction is the extension, done above */
  return base << shift;
}

uint32_t oemu_exec_internal_nz(uint64_t result, oemu_reg_width width) {
  const unsigned bits = reg_bits_of(width);
  const uint64_t value = (bits == 64U) ? result : (result & UINT32_MAX);
  uint32_t nzcv = 0U;
  if (value == 0U) {
    nzcv |= OEMU_NZCV_Z;
  }
  if ((value & (UINT64_C(1) << (bits - 1U))) != 0U) {
    nzcv |= OEMU_NZCV_N;
  }
  return nzcv;
}

uint64_t oemu_exec_internal_umulh(uint64_t a, uint64_t b) {
  /* Three visible partial products; the middle additions propagate carries
   * explicitly because C gives no 128-bit type to hide them in. */
  const uint64_t a_lo = a & UINT32_MAX;
  const uint64_t a_hi = a >> 32;
  const uint64_t b_lo = b & UINT32_MAX;
  const uint64_t b_hi = b >> 32;

  const uint64_t p0 = a_lo * b_lo;
  const uint64_t p1 = a_lo * b_hi;
  const uint64_t p2 = a_hi * b_lo;
  const uint64_t p3 = a_hi * b_hi;

  const uint64_t mid = (p0 >> 32) + (p1 & UINT32_MAX) + (p2 & UINT32_MAX);
  return p3 + (p1 >> 32) + (p2 >> 32) + (mid >> 32);
}

uint64_t oemu_exec_internal_smulh(uint64_t a, uint64_t b) {
  /* Signed high word = unsigned high word minus the two sign corrections. */
  uint64_t hi = oemu_exec_internal_umulh(a, b);
  if ((a & (UINT64_C(1) << 63)) != 0U) {
    hi -= b;
  }
  if ((b & (UINT64_C(1) << 63)) != 0U) {
    hi -= a;
  }
  return hi;
}

static uint64_t clz64_ref(uint64_t value) {
  if (value == 0U) {
    return 64U; /* the all-zero corner is what the loop below cannot express */
  }
  uint64_t n = 0U;
  for (unsigned bit = 63U;; --bit) {
    if (((value >> bit) & UINT64_C(1)) != 0U) {
      return n;
    }
    n++;
  }
}

uint64_t oemu_exec_internal_clz(uint64_t value, oemu_reg_width width) {
  if (width == OEMU_REG_W32) {
    const uint64_t low = value & UINT32_MAX;
    return (low == 0U) ? 32ULL : clz64_ref(low << 32); /* shift up: W sees 32 bits */
  }
  return clz64_ref(value);
}

uint64_t oemu_exec_internal_cls(uint64_t value, oemu_reg_width width) {
  const unsigned bits = reg_bits_of(width);
  const uint64_t mask = (bits == 64U) ? ~UINT64_C(0) : ((UINT64_C(1) << bits) - UINT64_C(1));
  const uint64_t v = value & mask;
  /* All-zero and all-one inputs have no prediction bits to report. */
  if (v == 0U || v == mask) {
    return 0U;
  }
  /* The general case is CLZ of the value XOR its replicated sign bit, minus
   * the sign bit itself -- which is exactly the count of leading bits that
   * match it. */
  const uint64_t sign = ((v >> (bits - 1U)) != 0U) ? mask : 0U;
  return oemu_exec_internal_clz(v ^ sign, width) - 1U;
}

uint64_t oemu_exec_internal_rbit(uint64_t value, oemu_reg_width width) {
  const unsigned bits = reg_bits_of(width);
  const uint64_t mask = (bits == 64U) ? ~UINT64_C(0) : ((UINT64_C(1) << bits) - UINT64_C(1));
  uint64_t out = 0U;
  for (unsigned i = 0U; i < bits; i++) {
    if ((value & (UINT64_C(1) << i)) != 0U) {
      out |= UINT64_C(1) << (bits - 1U - i);
    }
  }
  return out & mask;
}

uint64_t oemu_exec_internal_rev(uint64_t value, oemu_reg_width width) {
  const unsigned bits = reg_bits_of(width);
  uint64_t out = 0U;
  for (unsigned byte = 0U; byte < bits / 8U; byte++) {
    const uint64_t b = (value >> (8U * byte)) & UINT64_C(0xFF);
    out |= b << (8U * (bits / 8U - 1U - byte));
  }
  return out;
}

uint64_t oemu_exec_internal_rev16(uint64_t value, oemu_reg_width width) {
  const unsigned bits = reg_bits_of(width);
  uint64_t out = 0U;
  /* REV16 reverses the two bytes within each halfword of the result. */
  for (unsigned half = 0U; half < bits / 16U; half++) {
    const uint64_t h = (value >> (16U * half)) & UINT64_C(0xFFFF);
    const uint64_t swapped = ((h & UINT64_C(0xFF)) << 8U) | (h >> 8U);
    out |= swapped << (16U * half);
  }
  return out;
}

uint64_t oemu_exec_internal_rev32(uint64_t value) {
  const uint64_t lo = (value & UINT32_MAX) << 32;
  const uint64_t hi = value >> 32;
  return lo | hi;
}

/* --- dispatch internals ------------------------------------------------------- */

/* C source for a flag-setting logical: the shifter's carry, or the immediate
 * rule (an all-ones mask leaves C set), or the old C when neither applies. */
static uint32_t logical_flags(uint32_t old_nzcv, uint64_t result, const oemu_insn *in,
                              const oemu_exec_shift_result *shifted) {
  uint32_t nzcv = oemu_exec_internal_nz(result, in->width); /* V = 0 by architecture */
  bool carry = false;
  bool carry_valid = false;
  if (shifted != NULL) {
    carry_valid = shifted->carry_valid;
    carry = shifted->carry;
  } else if (in->operand_kind == OEMU_OPERAND_IMM) {
    const unsigned bits = reg_bits_of(in->width);
    const uint64_t mask = (bits == 64U) ? ~UINT64_C(0) : ((UINT64_C(1) << bits) - UINT64_C(1));
    carry = (in->uimm == mask); /* immh == 0b111100: the all-ones immediate */
    carry_valid = true;
  }
  if (carry_valid) {
    nzcv |= carry ? OEMU_NZCV_C : 0U;
  } else {
    nzcv |= old_nzcv & OEMU_NZCV_C;
  }
  return nzcv;
}

/* Second operand of add/sub and logical forms, whatever kind it is. */
static oemu_status operand2(const oemu_cpu *cpu, const oemu_insn *in, uint64_t *out,
                            oemu_exec_shift_result *shifted_out) {
  switch (in->operand_kind) {
    case OEMU_OPERAND_IMM:
      *out = in->uimm;
      return OEMU_OK;
    case OEMU_OPERAND_REG:
      *out = read_g(cpu, in->rm, false, in->width);
      return OEMU_OK;
    case OEMU_OPERAND_REG_SHIFTED:
      *shifted_out = oemu_exec_internal_shift_operand(
          read_g(cpu, in->rm, false, in->width), in->shift_type, in->shift_amount, in->width);
      *out = shifted_out->value;
      return OEMU_OK;
    case OEMU_OPERAND_REG_EXTENDED:
      *out = oemu_exec_internal_extend_operand(read_g(cpu, in->rm, false, OEMU_REG_W64),
                                               in->extend_type, in->shift_amount,
                                               in->extend_is_lsl);
      return OEMU_OK;
    case OEMU_OPERAND_NONE:
    case OEMU_OPERAND_MEM:
      return OEMU_ERR_DECODE; /* no second source exists for this kind */
  }
  return OEMU_ERR_DECODE;
}

/* Clears an overlapping reservation; the store side of the monitor rules. */
static void note_store(oemu_cpu *cpu, uint64_t addr, uint64_t nbytes) {
  if (!cpu->monitor_valid) {
    return;
  }
  /* Overlap test written as subtractions: the reserved range may touch 2^64. */
  const bool overlap = (cpu->monitor_addr <= addr)
                           ? ((addr - cpu->monitor_addr) < cpu->monitor_size)
                           : ((cpu->monitor_addr - addr) < nbytes);
  if (overlap) {
    cpu->monitor_valid = false;
  }
}

static oemu_status do_mrs(oemu_cpu *cpu, const oemu_sysregs *sr, const oemu_insn *in) {
  const uint32_t sel = in->sysreg & SYSREG_MASK;
  uint64_t value;
  switch (sel) {
    case SYSREG_CURRENT_EL:
      value = 0U; /* the model runs at EL0, which is what this register reports */
      break;
    case SYSREG_SP_EL0:
      value = oemu_regs_sp(&cpu->regs);
      break;
    case SYSREG_NZCV:
      value = oemu_regs_nzcv(&cpu->regs);
      break;
    case SYSREG_TPIDRUR_EL0:
      value = cpu->tpidrur_el0;
      break;
    case SYSREG_TPIDRRO_EL0:
      value = 0U; /* read-only, and nothing here sets it */
      break;
    default:
      /* Everything else -- FPCR/FPSR, the counters, the ID registers -- is the
       * sysreg table's business, and the table is where accessibility is
       * decided: a row that does not exist, or one whose min_el is above the
       * current EL, or a read-only row answers with a refusal, which is the
       * architecturally right answer (an Undefined instruction) and is what the
       * old flat whitelist did by hand for the five registers it knew. */
      if ((sr == NULL) || (oemu_sysreg_read(sr, sel, &value) != OEMU_OK)) {
        return OEMU_ERR_UNSUPPORTED;
      }
      break;
  }
  write_g(cpu, in->rd, false, OEMU_REG_W64, value);
  return OEMU_OK;
}

static oemu_status do_msr(oemu_cpu *cpu, oemu_sysregs *sr, const oemu_insn *in) {
  const uint32_t sel = in->sysreg & SYSREG_MASK;
  const uint64_t value = read_g(cpu, in->rd, false, OEMU_REG_W64);
  switch (sel) {
    case SYSREG_SP_EL0:
      oemu_regs_set_sp(&cpu->regs, value);
      break;
    case SYSREG_NZCV:
      oemu_regs_set_nzcv(&cpu->regs, (uint32_t)value);
      break;
    case SYSREG_TPIDRUR_EL0:
      cpu->tpidrur_el0 = value;
      break;
    default:
      /* The same fall-through as the read side: FPCR/FPSR and every other
       * writable row the table carries, with the table's own write mask,
       * min_el and read-only verdicts applied. TPIDRRO_EL0 has no row, so it
       * still refuses. */
      if ((sr == NULL) || (oemu_sysreg_write(sr, sel, value) != OEMU_OK)) {
        return OEMU_ERR_UNSUPPORTED;
      }
      break;
  }
  return OEMU_OK;
}

static oemu_status do_svc(oemu_cpu *cpu, const oemu_env_ops *env, const oemu_memops *mem) {
  if ((env == NULL) || (env->syscall == NULL)) {
    return OEMU_ERR_UNSUPPORTED;
  }
  uint64_t args[6];
  for (unsigned i = 0U; i < 6U; i++) {
    args[i] = oemu_regs_read(&cpu->regs, i, OEMU_REG_W64);
  }
  const uint64_t nr = oemu_regs_read(&cpu->regs, 8U, OEMU_REG_W64);
  const int64_t result = env->syscall(env->ctx, mem, nr, args);
  /* Linux returns in x0, 64 bits wide: an errno arrives sign-negative. */
  oemu_regs_write(&cpu->regs, 0U, OEMU_REG_W64, (uint64_t)result);
  return OEMU_OK;
}

/* Shared address computation for every non-literal load/store form. */
static oemu_status resolve_mem_addr(const oemu_cpu *cpu, const oemu_insn *in, uint64_t *addr,
                                    uint64_t *writeback) {
  const uint64_t base = read_g(cpu, in->rn, true, OEMU_REG_W64);
  uint64_t offset;
  if (in->extend_type != OEMU_EXTEND_UXTB || in->extend_is_lsl) {
    /* The register-offset form: the decoder only ever sets a real extend (or
     * the LSL variant) there, so UXTB-with-no-LSL means the immediate forms. */
    offset =
        oemu_exec_internal_extend_operand(read_g(cpu, in->rm, false, OEMU_REG_W64),
                                          in->extend_type, in->shift_amount, in->extend_is_lsl);
  } else {
    offset = (uint64_t)in->imm;
  }
  *writeback = base + offset;
  *addr = (in->index_mode == OEMU_INDEX_POST) ? base : base + offset;
  return OEMU_OK;
}

/* LDP/LDPSW/STP: second address is the first one plus one transfer. */
static uint64_t pair_transfer(const oemu_insn *in) {
  return UINT64_C(1) << (unsigned)in->mem_size;
}

static oemu_status do_pair(oemu_cpu *cpu, const oemu_memops *mem, const oemu_insn *in) {
  uint64_t addr;
  uint64_t writeback;
  (void)resolve_mem_addr(cpu, in, &addr, &writeback);
  const uint64_t addr2 = addr + pair_transfer(in);
  const uint64_t transfer = pair_transfer(in);

  /* Validate both transfers before touching anything, so a fault on the
   * second one cannot commit the first. */
  oemu_status st = mem->validate(mem->ctx, addr, transfer,
                                 (in->op == OEMU_OP_STP) ? OEMU_PERM_WRITE : OEMU_PERM_READ);
  if (st == OEMU_OK) {
    st = mem->validate(mem->ctx, addr2, transfer,
                       (in->op == OEMU_OP_STP) ? OEMU_PERM_WRITE : OEMU_PERM_READ);
  }
  if (st != OEMU_OK) {
    return OEMU_ERR_FAULT;
  }

  uint64_t v1 = 0U;
  uint64_t v2 = 0U;
  if (in->op == OEMU_OP_STP) {
    v1 = read_g(cpu, in->rd, false, in->width);
    v2 = read_g(cpu, in->rt2, false, in->width);
    access_or_panic(mem->write(mem->ctx, addr, in->mem_size, v1));
    access_or_panic(mem->write(mem->ctx, addr2, in->mem_size, v2));
    note_store(cpu, addr, transfer * 2U);
  } else {
    access_or_panic(mem->read(mem->ctx, addr, in->mem_size, in->is_signed_load, &v1));
    access_or_panic(mem->read(mem->ctx, addr2, in->mem_size, in->is_signed_load, &v2));
    write_g(cpu, in->rd, false, in->width, v1);
    write_g(cpu, in->rt2, false, in->width, v2);
  }
  if (in->index_mode != OEMU_INDEX_NONE) {
    write_g(cpu, in->rn, true, OEMU_REG_W64, writeback);
  }
  return OEMU_OK;
}

/*
 * LDP/STP over V0-V31: two registers, each `stride` bytes apart, where the
 * stride is the transfer size -- 4 for the S form, 8 for D, 16 for Q. A
 * 128-bit register is moved as two 64-bit bus accesses, so nothing new reaches
 * the bus, the MMU or a device; a narrower form writes the low bits and
 * clears the rest of the destination, which is what the architecture requires
 * rather than an optimisation.
 */
static oemu_status do_pair_vector(oemu_cpu *cpu, const oemu_memops *mem, const oemu_insn *in) {
  uint64_t addr = 0U;
  uint64_t writeback = 0U;
  (void)resolve_mem_addr(cpu, in, &addr, &writeback);
  const oemu_mem_size piece = (in->mem_size == OEMU_MEM_128) ? OEMU_MEM_DWORD : in->mem_size;
  const uint64_t stride = UINT64_C(1) << (unsigned)in->mem_size;
  const unsigned pieces = (in->mem_size == OEMU_MEM_128) ? 2U : 1U;
  const uint64_t bytes = (stride < 8U) ? stride : 8U; /* the two-S form moves 4 */
  const bool is_store = (in->op == OEMU_OP_STP);
  const uint32_t perm = is_store ? OEMU_PERM_WRITE : OEMU_PERM_READ;

  /* Every piece of both registers is validated first, so an abort leaves the
   * pair, the register file and the base register untouched. */
  for (unsigned r = 0U; r < 2U; r++) {
    for (unsigned p = 0U; p < pieces; p++) {
      if (mem->validate(mem->ctx, addr + r * stride + p * 8U, bytes, perm) != OEMU_OK) {
        return OEMU_ERR_FAULT;
      }
    }
  }

  for (unsigned r = 0U; r < 2U; r++) {
    const unsigned reg = (r == 0U) ? in->rd : in->rt2;
    const uint64_t a = addr + r * stride;
    uint64_t lo = 0U;
    uint64_t hi = 0U;
    if (is_store) {
      lo = cpu->v[reg][0];
      hi = cpu->v[reg][1];
      access_or_panic(mem->write(mem->ctx, a, piece, lo));
      if (pieces == 2U) {
        access_or_panic(mem->write(mem->ctx, a + 8U, OEMU_MEM_DWORD, hi));
      }
    } else {
      access_or_panic(mem->read(mem->ctx, a, piece, false, &lo));
      if (pieces == 2U) {
        access_or_panic(mem->read(mem->ctx, a + 8U, OEMU_MEM_DWORD, false, &hi));
      }
      cpu->v[reg][0] = lo; /* a sub-128-bit transfer zeroes the rest */
      cpu->v[reg][1] = hi;
    }
  }
  if (is_store) {
    note_store(cpu, addr, stride * 2U);
  }
  if (in->index_mode != OEMU_INDEX_NONE) {
    write_g(cpu, in->rn, true, OEMU_REG_W64, writeback);
  }
  return OEMU_OK;
}

static oemu_status do_single_mem(oemu_cpu *cpu, const oemu_memops *mem, const oemu_insn *in) {
  const bool is_store = (in->op == OEMU_OP_STR || in->op == OEMU_OP_STLR);
  const oemu_mem_size size = in->mem_size;
  const uint64_t nbytes = UINT64_C(1) << (unsigned)size;
  uint64_t addr;
  uint64_t writeback = 0U;
  bool has_writeback = false;

  if (in->operand_kind == OEMU_OPERAND_IMM) {
    addr = (uint64_t)in->imm; /* LDR literal: already PC-resolved by the decoder */
  } else {
    (void)resolve_mem_addr(cpu, in, &addr, &writeback);
    has_writeback = (in->index_mode != OEMU_INDEX_NONE);
  }

  const oemu_status st =
      mem->validate(mem->ctx, addr, nbytes, is_store ? OEMU_PERM_WRITE : OEMU_PERM_READ);
  if (st != OEMU_OK) {
    return OEMU_ERR_FAULT;
  }

  uint64_t value = 0U;
  if (is_store) {
    value = read_g(cpu, in->rd, false, in->width);
    access_or_panic(mem->write(mem->ctx, addr, size, value));
    note_store(cpu, addr, nbytes);
  } else {
    access_or_panic(mem->read(mem->ctx, addr, size, in->is_signed_load, &value));
    write_g(cpu, in->rd, false, in->width, value);
  }
  if (has_writeback) {
    write_g(cpu, in->rn, true, OEMU_REG_W64, writeback);
  }
  return OEMU_OK;
}

static oemu_status do_exclusive(oemu_cpu *cpu, const oemu_memops *mem, const oemu_insn *in) {
  const uint64_t nbytes = UINT64_C(1) << (unsigned)in->mem_size;
  const bool is_load = (in->op == OEMU_OP_LDXR);
  const uint64_t addr = read_g(cpu, in->rn, true, OEMU_REG_W64);

  const oemu_status st =
      mem->validate(mem->ctx, addr, nbytes, is_load ? OEMU_PERM_READ : OEMU_PERM_WRITE);
  if (st != OEMU_OK) {
    return OEMU_ERR_FAULT;
  }

  if (is_load) {
    uint64_t value = 0U;
    access_or_panic(mem->read(mem->ctx, addr, in->mem_size, false, &value));
    write_g(cpu, in->rd, false, in->width, value);
    cpu->monitor_addr = addr;
    cpu->monitor_size = nbytes;
    cpu->monitor_valid = true;
    return OEMU_OK;
  }

  /* STXR: success means a live reservation on exactly this range. The status
   * register is always W32-sized, whatever the access width. */
  const bool success =
      cpu->monitor_valid && (cpu->monitor_addr == addr) && (cpu->monitor_size == nbytes);
  cpu->monitor_valid = false;
  if (success) {
    const uint64_t value = read_g(cpu, in->rd, false, in->width);
    access_or_panic(mem->write(mem->ctx, addr, in->mem_size, value));
  }
  write_g(cpu, in->rm, false, OEMU_REG_W32, success ? UINT64_C(0) : UINT64_C(1));
  return OEMU_OK;
}

/* SBFM/UBFM/BFM share lsb/msb extraction; the >-inversion quirk is per-op. */
static oemu_status do_bitfield(oemu_cpu *cpu, const oemu_insn *in) {
  const unsigned bits = reg_bits_of(in->width);
  const unsigned lsb = in->shift_amount; /* immR */
  const unsigned msb = in->bit_pos;      /* immS */
  if (lsb >= bits || msb >= bits) {
    return OEMU_ERR_DECODE; /* the decoder already rejects this; be exact anyway */
  }

  const uint64_t width_mask =
      (bits == 64U) ? ~UINT64_C(0) : ((UINT64_C(1) << bits) - UINT64_C(1));
  const uint64_t src = read_g(cpu, in->rn, false, in->width);

  /* ARM gives SBFM/UBFM as "take the low `len` bits of ROR(Xn, #immR)", where
   * a wrapped range (immR > immS) shortens len instead of erroring. Note the
   * wrap adds back the two pieces' overlap-free count: len = regsize - immR +
   * immS + 1, NOT regsize - immR + immS. Getting that off by one truncates
   * e.g. UBFIZ #3,#9 (the wrapping spelling of `lsl #3` on a 9-bit index) to
   * 11 bits -- which drops the top bit of a PMD index and lands a page-table
   * walk in the wrong slot. */
  const unsigned len = (msb < lsb) ? (bits - lsb + msb + 1U) : (msb - lsb + 1U);
  uint64_t rot;
  if (msb < lsb) {
    /* Wrapped range (immR > immS): this is the shift-alias form, where UBFM
     * spells `lsl #s` (immR=regsize-s, immS=regsize-1-s) and SBFM the
     * sign-extending flavour. A wrapped UBFM is a LEFT SHIFT by
     * (regsize - immR), NOT a rotate: the ARM field spans bits immS:0 and
     * regsize-1:immR, and when extracted right-aligned those two pieces sit
     * contiguously at the top with the vacated low `immR` bits forced to zero.
     * A rotate instead folds the source's top (regsize-immR) bits back into
     * those low positions. That is not cosmetic: `lsl x,x,#12` (UBFM #52,#51)
     * is the very instruction Linux's `allocate_slab` uses to form a slab
     * object address, and leaving the wrapped bits in returns `...fff` where
     * the address must end `...000` -- every object one cacheline-short, the
     * freelist links land misaligned, and the first vmap-tree walk dies on a
     * garbage rb_right. */
    rot = (src << (bits - lsb)) & width_mask;
  } else if (lsb == 0U) {
    rot = src;
  } else {
    rot = (src >> lsb) | (src << (bits - lsb));
    rot &= width_mask;
  }
  const uint64_t mask = (len == bits) ? width_mask : ((UINT64_C(1) << len) - UINT64_C(1));
  uint64_t field = rot & mask;

  if (in->op == OEMU_OP_BFM) {
    /*
     * BFM is UBFM's extracted value *merged into* the destination instead of
     * overwriting it, and it lands where that value already sits -- not at
     * `immR`. A non-wrapped range right-aligns the extract, so the field is
     * [len-1:0]; that is BFXIL Xd,Xn,#lsb,#width, whose definition (Xd<width-1:
     * 0> = Xn<lsb+width-1:lsb>) is exactly this rule. A wrapped range has been
     * shifted up by regsize-immR, so the field is [len-1:regsize-immR], clipped
     * to the register by construction because a wrapped range has len <=
     * regsize. Either way: mask the field, leave every other destination bit
     * alone.
     *
     * The old code refused to play at all. It took a wrapped BFM -- the
     * spelling every real `bfi` uses, since BFI Xd,Xn,#lsb,#width encodes as
     * immR=regsize-lsb, immS=width-1 -- for the reserved no-op it superficially
     * resembles, and inserted at `immR` in the non-wrapped case. That is issue
     * #26: `bfi x2, x0, #32, #32` is the splice inside lib/lockref.c's
     * lockref_get, so the instruction vanished, __cmpxchg_case_64 stored the
     * OLD packed word back, the dentry's refcount never went 1 -> 2, the very
     * next dput killed and RCU-freed a dentry the open struct still pointed at,
     * and Linux read d_inode == 0 out of the freed object and died in
     * chown_common+0x48 with FAR=0x28. Only regular files trip it: they are the
     * only initramfs entries whose populate path takes that dget/dput pair.
     */
    const unsigned bottom = (msb < lsb) ? (bits - lsb) : 0U;
    const unsigned fbits = len - bottom; /* wrapped: msb + 1; non-wrapped: len */
    const uint64_t dm =
        (fbits == bits) ? width_mask : (((UINT64_C(1) << fbits) - UINT64_C(1)) << bottom);
    const uint64_t old = read_g(cpu, in->rd, false, in->width);
    field = (old & ~dm) | (field & dm);
  } else if ((in->op == OEMU_OP_SBFM) && (((field >> (len - 1U)) & UINT64_C(1)) != 0U)) {
    /* Sign-extend inside the register width, then let write_g truncate. */
    field |= width_mask & ~mask;
  }
  write_g(cpu, in->rd, false, in->width, field);
  return OEMU_OK;
}

uint32_t oemu_exec_internal_crc32(uint32_t crc_in, uint64_t data, unsigned bytes) {
  /* The architecture's CRC() pseudocode verbatim: XOR the running sum's MSB
   * with the incoming data LSB, shift the sum left, feed the data right, and
   * fold in the polynomial where they differ. LSB-first => the reflected
   * CRC-32 the guest's crc32() returns. */
  const uint32_t poly = 0x04C11DB7U;
  uint32_t crc = crc_in;
  const unsigned nbits = bytes * 8U;
  for (unsigned j = 0U; j < nbits; ++j) {
    const uint32_t topbit = ((crc >> 31) ^ (uint32_t)(data & UINT64_C(1))) & UINT32_C(1);
    crc <<= 1;
    data >>= 1;
    if (topbit != 0U) {
      crc ^= poly;
    }
  }
  return crc;
}

static oemu_status do_csel(oemu_cpu *cpu, const oemu_insn *in) {
  const uint64_t a = read_g(cpu, in->rn, false, in->width);
  const uint64_t b = read_g(cpu, in->rm, false, in->width);
  uint64_t out;
  if (oemu_regs_cond_holds(&cpu->regs, in->cond)) {
    out = a;
  } else if (in->op == OEMU_OP_CSINC) {
    out = b + UINT64_C(1);
  } else if (in->op == OEMU_OP_CSINV) {
    out = ~b;
  } else if (in->op == OEMU_OP_CSNEG) {
    out = 0U - b;
  } else {
    out = b;
  }
  write_g(cpu, in->rd, false, in->width, out);
  return OEMU_OK;
}

static oemu_status do_ccmp(oemu_cpu *cpu, const oemu_insn *in) {
  if (!oemu_regs_cond_holds(&cpu->regs, in->cond)) {
    oemu_regs_set_nzcv(&cpu->regs, (uint32_t)(in->uimm << 28));
    return OEMU_OK;
  }
  const uint64_t n = read_g(cpu, in->rn, false, in->width);
  const uint64_t m = (in->operand_kind == OEMU_OPERAND_IMM)
                         ? (uint64_t)in->imm
                         : read_g(cpu, in->rm, false, in->width);
  const oemu_alu_result res = (in->op == OEMU_OP_CCMP)
                                  ? oemu_regs_add_with_carry(n, ~m, true, in->width)
                                  : oemu_regs_add_with_carry(n, m, false, in->width);
  oemu_regs_set_nzcv(&cpu->regs, res.nzcv);
  return OEMU_OK;
}

/* --- system-mode (M2c) ---------------------------------------------------------- */
/*
 * The system dispatch adds no new instruction semantics: it answers the same
 * opcodes at EL1+ where the architecture says so -- SVC/BRK/HLT/HVC/SMC trap
 * into oemu/exc.h instead of returning a host-visible status, MRS/MSR go
 * through the sysreg table instead of the EL0 whitelist, ERET returns, and a
 * memory fault becomes a Data Abort with a FAR. Everything arithmetic or
 * plain-memory stays in the shared switch below, so user mode and system mode
 * cannot drift apart there.
 */

/* The op0=0b01 trap space is keyed by op1/CRn/CRm/op2; SEL carries them as
 * [13:11]/[10:7]/[6:3]/[2:0] (the same split the sysreg rows use). */
#define SYS_OP1(s) (((s) >> 11U) & 0x7U)
#define SYS_CRN(s) (((s) >> 7U) & 0xFU)
#define SYS_CRM(s) (((s) >> 3U) & 0xFU)
#define SYS_OP2(s) ((s) & 0x7U)

/* DC ZVA: op0=1 op1=3 CRn=7 CRm=4 op2=1 (Linux SYS_DC_ZVA, confirmed by the
 * clang-harvested `dc zva, x3` = 0xD50B7423). */
#define SYS_DC_ZVA ((uint32_t)0x1BA1U)
/* A modelled cache line: matches the 64-byte line CCSIDR/CLIDR/CTR advertise. */
#define EXEC_CACHE_LINE 64U

oemu_exec_sys_action oemu_exec_internal_sys_action(uint32_t sel) {
  const unsigned op1 = SYS_OP1(sel);
  const unsigned crn = SYS_CRN(sel);
  const unsigned crm = SYS_CRM(sel);

  if (sel == SYS_DC_ZVA) {
    return OEMU_EXEC_SYS_DC_ZVA;
  }
  /* TLBI, all privilege banks (op1 in {0,1,2,4}, CRn 8/9): since M3b
   * there is a TLB, and an invalidation request must invalidate. oemu
   * answers every TLBI -- by-VA, by-ASID, and the CRn 9 stage-2 space,
   * whose evictions a stage-1 flush-all satisfies -- with the whole
   * cache: precise invalidation is a deliberate later step, and
   * flush-all is the honest superset until then. Unallocated holes in
   * the space execute as no-ops too -- harmless while the space decodes
   * as one class. */
  if (((crn == 8U) || (crn == 9U)) &&
      ((op1 == 0U) || (op1 == 1U) || (op1 == 2U) || (op1 == 4U))) {
    return OEMU_EXEC_SYS_TLBI;
  }
  /* AT (op1 in {0,4}, CRn 7, CRm 8/9: the full S1E and S12E table of Linux
   * asm/sysreg.h): a translation request needs the MMU (M3). Stage-1 AT
   * (op1 == 0, the S1E ops the running EL1 kernel probes with) is honoured by
   * walking and publishing the result in PAR_EL1. Stage-2 AT (op1 == 4,
   * the S12E ops) has no stage-2 translation to perform here, so it stays
   * Undefined -- executing it as a no-op would silently lie about PAR_EL1. */
  if ((crn == 7U) && ((crm == 8U) || (crm == 9U)) && (op1 == 0U)) {
    return OEMU_EXEC_SYS_AT;
  }
  if ((crn == 7U) && ((crm == 8U) || (crm == 9U)) && (op1 == 4U)) {
    return OEMU_EXEC_SYS_TRAP;
  }
  /* DC and IC invalidation/clean space (CRn 7/10/11/15 with op1 in {0,3}):
   * oemu models no caches and guest memory is the coherence point, so
   * maintain-without-invalidate and invalidate are both honest no-ops. */
  if ((op1 == 0U || op1 == 3U) &&
      ((crn == 7U) || (crn == 10U) || (crn == 11U) || (crn == 15U))) {
    return OEMU_EXEC_SYS_NOP;
  }
  /* Everything else -- AT variants above, SYS G0..G7 random generators,
   * unallocated holes: not implemented, and the architecture's answer to an
   * encoding the machine does not have is Undefined. */
  return OEMU_EXEC_SYS_TRAP;
}

/* Re-assemble an op0=0b01 trap-space word from its selector and Rt. The
 * caller normally has the fetched word already and should pass it; this
 * exists for synthetic dispatch (white-box tests) that hand-builds an
 * oemu_insn. Bits [29:21] are the group key (verified against the harvested
 * encodings); SEL rides at [18:5], Rt at [4:0]. */
uint32_t oemu_exec_internal_reencode_sys(uint32_t sel, unsigned rt) {
  return 0xD5000000U | (1U << 19) | ((sel & SYSREG_MASK) << 5) | (rt & 0x1FU);
}

/* The address a memory instruction touched, for the Data Abort FAR. Only
 * legal after a FAULT: the dispatch is precise, so the address inputs are
 * still untouched and the same computation the access made can be redone. */
static oemu_status fault_far_of(const oemu_cpu *cpu, const oemu_insn *in, uint64_t *far_out) {
  if (in->operand_kind == OEMU_OPERAND_IMM) {
    *far_out = (uint64_t)in->imm; /* LDR (literal): already PC-resolved */
    return OEMU_OK;
  }
  if ((in->op == OEMU_OP_LDXR) || (in->op == OEMU_OP_STXR)) {
    *far_out = read_g(cpu, in->rn, true, OEMU_REG_W64); /* base form, no offset */
    return OEMU_OK;
  }
  uint64_t writeback = 0U;
  (void)resolve_mem_addr(cpu, in, far_out, &writeback);
  return OEMU_OK;
}

/* WnR for the data abort ISS: which memory ops transfer out of the core. */
static bool op_is_store(oemu_opcode op) {
  return (op == OEMU_OP_STR) || (op == OEMU_OP_STP) || (op == OEMU_OP_STLR) ||
         (op == OEMU_OP_STXR);
}

/* MRS through the table. An encoding the table refuses (absent row, too low
 * an EL for the row, read-only in the wrong direction) is the architecture's
 * Undefined instruction, carrying the fetched encoding as ISS. */
static oemu_status do_mrs_system(oemu_cpu *cpu, oemu_sysregs *sr, const oemu_insn *in,
                                 uint32_t word) {
  const uint32_t sel = in->sysreg & SYSREG_MASK;
  uint64_t value = 0U;
  if (oemu_sysreg_read(sr, sel, &value) != OEMU_OK) {
    oemu_exc_undefined(&cpu->regs, sr, word);
    return OEMU_ERR_FAULT; /* delivered: the step consumed the instruction */
  }
  write_g(cpu, in->rd, false, OEMU_REG_W64, value);
  return OEMU_OK;
}

static oemu_status do_msr_system(oemu_cpu *cpu, oemu_sysregs *sr, const oemu_insn *in,
                                 uint32_t word) {
  const uint32_t sel = in->sysreg & SYSREG_MASK;
  const uint64_t value = read_g(cpu, in->rd, false, OEMU_REG_W64);
  if (oemu_sysreg_write(sr, sel, value) != OEMU_OK) {
    oemu_exc_undefined(&cpu->regs, sr, word);
    return OEMU_ERR_FAULT;
  }
  return OEMU_OK;
}

/* MSR (immediate): SPSel (bank switch) and the DAIF masks are the only bits
 * with architectural effect; PAN/DIT/SSBS/UAIR are modelled as no-ops (the ID
 * registers honestly advertise those features absent). A reserved selector is
 * Undefined. Selector = op2 (insn.uimm), imm4 = insn.imm, both set by the
 * decoder from bits 7:5 and 11:8. */
#define MSR_IMM_SPSel   5U
#define MSR_IMM_DAIFSET 6U
#define MSR_IMM_DAIFCLR 7U
static oemu_status do_msr_immediate(oemu_cpu *cpu, oemu_sysregs *sr, const oemu_insn *in,
                                    uint32_t word) {
  const uint32_t op2 = (uint32_t)in->uimm;
  const uint32_t imm = ((uint32_t)in->imm) & 0xfU;
  oemu_status st = OEMU_OK;
  switch (op2) {
    case MSR_IMM_SPSel:
      st = oemu_sysreg_write(sr, OEMU_SYSREG_SPSEL, (uint64_t)(imm & 1U));
      break;
    case MSR_IMM_DAIFSET:
    case MSR_IMM_DAIFCLR: {
      uint64_t daif = 0U;
      if (oemu_sysreg_read(sr, OEMU_SYSREG_DAIF, &daif) != OEMU_OK) {
        return OEMU_ERR_FAULT;
      }
      /* The immediate forms carry a compact 4-bit field (D=bit3 .. F=bit0)
       * while the DAIF register itself uses the PSTATE positions, so shift. */
      const uint64_t mask = ((uint64_t)imm & 0xfU) << OEMU_PSTATE_DAIF_SHIFT;
      const uint64_t next = (op2 == MSR_IMM_DAIFSET) ? (daif | mask) : (daif & ~mask);
      st = oemu_sysreg_write(sr, OEMU_SYSREG_DAIF, next);
      break;
    }
    case 1U: /* SSBS */
    case 2U: /* DIT  */
    case 3U: /* UAIR */
    case 4U: /* PAN  */
      break; /* modelled as no-op */
    default: /* reserved selector */
      oemu_exc_undefined(&cpu->regs, sr, word);
      return OEMU_ERR_FAULT;
  }
  return (st == OEMU_OK) ? OEMU_OK : OEMU_ERR_FAULT;
}

/* SYS op: DC/IC/TLBI policy is the classifier's; a trap is an Undefined with
 * the fetched encoding as ISS. */
static oemu_status do_sys(oemu_cpu *cpu, oemu_sysregs *sr, const oemu_memops *mem,
                          oemu_mmu *mmu, const oemu_insn *in, uint32_t word) {
  const uint32_t sel = in->sysreg & SYSREG_MASK;
  switch (oemu_exec_internal_sys_action(sel)) {
    case OEMU_EXEC_SYS_NOP:
      return OEMU_OK;
    case OEMU_EXEC_SYS_TRAP:
      oemu_exc_undefined(&cpu->regs, sr, word);
      return OEMU_ERR_FAULT;
    case OEMU_EXEC_SYS_TLBI:
      /* The request is real and the answer is the whole cache. The mmu is
       * NULL exactly when the bus is the physical one -- a machine with
       * no translation layer has no translations to invalidate, so the
       * request is satisfied by having nothing to do. */
      if (mmu != NULL) {
        oemu_mmu_flush_all(mmu);
      }
      return OEMU_OK;
    case OEMU_EXEC_SYS_AT: {
      /* AT (stage-1): translate [Rt] in the current regime and publish the
       * verdict in PAR_EL1 -- bit 0 (F) set means the translation failed,
       * clear means it succeeded and the output address is carried in the
       * upper fields. The kernel's idmap/feature probes are `at s1e1r;
       * mrs par; tbnz par, #0`, so only F is consulted; the walk's own
       * fault (permission, translation, ...) is folded into F, which is all
       * PAR_EL1 promises for a fault. A read-vs-write probe differs only in
       * the write forms (op2 1/3). */
      const uint64_t at_addr = read_g(cpu, in->rd, false, OEMU_REG_W64);
      const uint32_t at_op2 = (word >> 5) & 0x7U;
      const bool at_write = (at_op2 == 1U) || (at_op2 == 3U);
      uint64_t at_pa = 0U;
      oemu_mmu_fault at_fault;
      const oemu_status at_st =
          (mmu != NULL) ? oemu_mmu_translate(mmu, at_addr, at_write, false, &at_pa, &at_fault)
                        : OEMU_ERR_FAULT;
      sr->par_el1 = (at_st == OEMU_OK) ? (at_pa & ~(uint64_t)1U) : 1U;
      return OEMU_OK;
    }
    case OEMU_EXEC_SYS_DC_ZVA:
      break;
  }

  /* DC ZVA: the address must be 8-byte aligned. The check is not gated on
   * SCTLR.A -- a DC ZVA is architecturally specified to fault when unaligned
   * -- and it reports as an Alignment fault: the data-abort EC with DFSC
   * 0x21, no ISS (the width of a cache line is not a transfer size). */
  const uint64_t addr = read_g(cpu, in->rd, false, OEMU_REG_W64);
  if ((addr & (UINT64_C(8) - UINT64_C(1))) != 0U) {
    oemu_exc_data_abort(&cpu->regs, sr, addr, 3U, true, false, 0x21U);
    return OEMU_ERR_FAULT;
  }
  const uint64_t line = addr & ~(uint64_t)(EXEC_CACHE_LINE - 1U);
  if (mem->validate(mem->ctx, line, EXEC_CACHE_LINE, OEMU_PERM_WRITE) != OEMU_OK) {
    /* The access could not be committed: the walk's record says which part
     * (permission, translation, missing backing); the bus's own refusal says
     * only that no mapping covers it. */
    oemu_mmu_fault fault;
    if ((mmu != NULL) && oemu_mmu_take_fault(mmu, &fault)) {
      oemu_exc_take(&cpu->regs, sr, OEMU_EXC_KIND_SYNC,
                    oemu_exc_route(oemu_pstate_el(sr->pstate)), fault.esr, fault.far, true);
    } else {
      oemu_exc_data_abort(&cpu->regs, sr, line, 3U, true, false, 0x2CU);
    }
    return OEMU_ERR_FAULT;
  }
  for (unsigned off = 0U; off < EXEC_CACHE_LINE; off += 8U) {
    /* Validated above; a validated access cannot fail (no provider re-shapes
     * between calls), so a refusal here is a bug, not a guest event. */
    access_or_panic(mem->write(mem->ctx, line + off, OEMU_MEM_DWORD, 0U));
  }
  return OEMU_OK;
}

/*
 * One instruction, at the current exception level, with system-mode
 * semantics. `word` is the fetched encoding (used as the Undefined ISS when
 * an instruction is refused, so the guest handler sees what hardware
 * reports). Every fault the architecture would deliver is delivered here
 * (via oemu/exc.h) and the function returns OEMU_OK so the run loop never
 * has to know the difference between "instruction executed" and "exception
 * taken": both are progress. A WFI/WFE with nothing to wake for returns
 * OEMU_ERR_BLOCKED without moving the PC. No allocation, no environment.
 */
oemu_status oemu_exec_internal_dispatch_system(oemu_cpu *cpu, oemu_sysregs *sr,
                                               const oemu_memops *mem, const oemu_insn *in,
                                               uint32_t word, oemu_mmu *mmu,
                                               const oemu_env_ops *env) {
  if ((cpu == NULL) || (sr == NULL) || (in == NULL) || (in->op == OEMU_OP_UNKNOWN)) {
    return OEMU_ERR_INVALID_ARG;
  }
  if ((mem == NULL) || (mem->fetch32 == NULL) || (mem->read == NULL) || (mem->write == NULL) ||
      (mem->validate == NULL)) {
    return OEMU_ERR_INVALID_ARG;
  }

  /* An if-chain, not a switch: -Wswitch-enum would demand every opcode be
   * named here even though everything not listed is deliberately left to the
   * shared switch below. */
  if (in->op == OEMU_OP_SVC) {
    oemu_exc_svc(&cpu->regs, sr, (uint16_t)in->imm);
    return OEMU_OK;
  }
  if (in->op == OEMU_OP_BRK) {
    oemu_exc_brk(&cpu->regs, sr, (uint16_t)in->imm);
    return OEMU_OK;
  }
  if (in->op == OEMU_OP_HLT) {
    oemu_exc_breakpoint(&cpu->regs, sr);
    return OEMU_OK;
  }
  if (in->op == OEMU_OP_HVC || in->op == OEMU_OP_SMC) {
    /* A firmware conduit first: an environment that answers PSCI (the
     * boot path) consumes the call and answers in x0; without one these
     * instructions stay the exceptions the architecture describes. */
    uint64_t ret0 = 0U;
    const uint64_t args[3] = {oemu_regs_read(&cpu->regs, 0, OEMU_REG_W64),
                              oemu_regs_read(&cpu->regs, 1, OEMU_REG_W64),
                              oemu_regs_read(&cpu->regs, 2, OEMU_REG_W64)};
    if ((env != NULL) && (env->fw_call != NULL) &&
        env->fw_call(env->ctx, in->op == OEMU_OP_HVC, (uint16_t)in->imm, args, &ret0)) {
      oemu_regs_write(&cpu->regs, 0, OEMU_REG_W64, ret0);
      oemu_regs_set_pc(&cpu->regs, oemu_regs_pc(&cpu->regs) + OEMU_INSN_SIZE);
      return OEMU_OK;
    }
    if (in->op == OEMU_OP_HVC) {
      oemu_exc_hvc(&cpu->regs, sr, (uint16_t)in->imm);
    } else {
      oemu_exc_smc(&cpu->regs, sr, (uint16_t)in->imm);
    }
    return OEMU_OK;
  }
  if (in->op == OEMU_OP_ERET) {
    /* oemu_exc_eret gates at EL0 itself (Undefined there). */
    oemu_exc_eret(&cpu->regs, sr);
    return OEMU_OK;
  }
  oemu_status sys = OEMU_OK;
  bool intercept = true;
  if (in->op == OEMU_OP_MRS) {
    sys = do_mrs_system(cpu, sr, in, word);
  } else if (in->op == OEMU_OP_MSR) {
    sys = do_msr_system(cpu, sr, in, word);
  } else if (in->op == OEMU_OP_MSR_IMM) {
    sys = do_msr_immediate(cpu, sr, in, word);
  } else if (in->op == OEMU_OP_SYS) {
    sys = do_sys(cpu, sr, mem, mmu, in, word);
  } else {
    intercept = false;
  }
  if (intercept) {
    if (sys == OEMU_ERR_FAULT) {
      return OEMU_OK; /* an exception was delivered: PC is the vector */
    }
    if (sys != OEMU_OK) {
      return sys; /* INVALID_ARG: a caller bug */
    }
    oemu_regs_advance_pc(&cpu->regs);
    return OEMU_OK;
  }

  /* WFI/WFE reach the shared switch and execute as no-ops here: the
   * wake-or-park decision belongs to the vCPU, which alone sees the
   * interrupt pins and the event register. */
  oemu_status st = oemu_exec_internal_dispatch_bus(cpu, mem, NULL, in);
  if (st == OEMU_ERR_FAULT) {
    /* Precise contract: the faulting instruction committed nothing. The
     * translation layer records the walk's verdict (class, level, the
     * virtual FAR) on the way out; prefer it. With no mmu, or a fault that
     * never reached the walk, the bus itself refused: DFSC 0x2C, the
     * translation fault with no level, at the recomputed address. */
    oemu_mmu_fault fault;
    if ((mmu != NULL) && oemu_mmu_take_fault(mmu, &fault)) {
      oemu_exc_take(&cpu->regs, sr, OEMU_EXC_KIND_SYNC,
                    oemu_exc_route(oemu_pstate_el(sr->pstate)), fault.esr, fault.far, true);
    } else {
      uint64_t far = 0U;
      (void)fault_far_of(cpu, in, &far);
      const bool is_write = op_is_store(in->op);
      const bool is_pair =
          (in->op == OEMU_OP_LDP) || (in->op == OEMU_OP_STP) || (in->op == OEMU_OP_LDPSW);
      oemu_exc_data_abort(&cpu->regs, sr, far, (unsigned)in->mem_size, is_write, !is_pair,
                          0x2CU);
    }
    return OEMU_OK;
  }
  if (st == OEMU_ERR_UNSUPPORTED) {
    /* An encoding outside the implemented subset (SIMD, atomics, ...): with
     * the ID registers honestly advertising them absent, the architectural
     * answer is Undefined, which is what QEMU delivers for disabled
     * features too. */
    oemu_exc_undefined(&cpu->regs, sr, word);
    return OEMU_OK;
  }
  return st;
}

/* --- the switch ---------------------------------------------------------------- */

oemu_status oemu_exec_internal_dispatch_bus(oemu_cpu *cpu, const oemu_memops *mem,
                                            const oemu_env_ops *env, const oemu_insn *in) {
  if (cpu == NULL || in == NULL || in->op == OEMU_OP_UNKNOWN) {
    return OEMU_ERR_INVALID_ARG;
  }
  /* A half-built view is a caller bug, and a NULL callback would otherwise
   * surface far from where it was introduced. */
  if ((mem == NULL) || (mem->fetch32 == NULL) || (mem->read == NULL) || (mem->write == NULL) ||
      (mem->validate == NULL)) {
    return OEMU_ERR_INVALID_ARG;
  }

  oemu_status st = OEMU_OK;
  bool take_branch = false;
  uint64_t branch_target = 0U;
  bool link = false;

  switch (in->op) {
    case OEMU_OP_UNKNOWN:
      return OEMU_ERR_INVALID_ARG;

    case OEMU_OP_ADD:
    case OEMU_OP_SUB:
    case OEMU_OP_ADDS:
    case OEMU_OP_SUBS: {
      uint64_t m = 0U; /* written by operand2 below; init for the analyzer */
      oemu_exec_shift_result shifted = {0U, false, false};
      (void)operand2(cpu, in, &m, &shifted);
      const uint64_t n = read_g(cpu, in->rn, in->rn_is_sp_form, in->width);
      const bool sub = (in->op == OEMU_OP_SUB) || (in->op == OEMU_OP_SUBS);
      const oemu_alu_result r = oemu_regs_add_with_carry(n, sub ? ~m : m, sub, in->width);
      write_g(cpu, in->rd, in->rd_is_sp_form, in->width, r.value);
      if (in->sets_flags) {
        oemu_regs_set_nzcv(&cpu->regs, r.nzcv);
      }
      break;
    }

    case OEMU_OP_ADC:
    case OEMU_OP_ADCS: {
      const uint64_t m = read_g(cpu, in->rm, false, in->width);
      const uint64_t n = read_g(cpu, in->rn, false, in->width);
      const bool c = (oemu_regs_nzcv(&cpu->regs) & OEMU_NZCV_C) != 0U;
      const oemu_alu_result r = oemu_regs_add_with_carry(n, m, c, in->width);
      write_g(cpu, in->rd, false, in->width, r.value);
      if (in->op == OEMU_OP_ADCS) {
        oemu_regs_set_nzcv(&cpu->regs, r.nzcv);
      }
      break;
    }
    case OEMU_OP_SBC:
    case OEMU_OP_SBCS: {
      /* SBC = n + ~m + C: the carry-in is the old C, not its complement. */
      const uint64_t m = read_g(cpu, in->rm, false, in->width);
      const uint64_t n = read_g(cpu, in->rn, false, in->width);
      const bool c = (oemu_regs_nzcv(&cpu->regs) & OEMU_NZCV_C) != 0U;
      const oemu_alu_result r = oemu_regs_add_with_carry(n, ~m, c, in->width);
      write_g(cpu, in->rd, false, in->width, r.value);
      if (in->op == OEMU_OP_SBCS) {
        oemu_regs_set_nzcv(&cpu->regs, r.nzcv);
      }
      break;
    }

    case OEMU_OP_AND:
    case OEMU_OP_ANDS:
    case OEMU_OP_ORR:
    case OEMU_OP_EOR:
    case OEMU_OP_BIC:
    case OEMU_OP_BICS:
    case OEMU_OP_ORN:
    case OEMU_OP_EON: {
      uint64_t m = 0U; /* written by operand2 below; init for the analyzer */
      oemu_exec_shift_result shifted = {0U, false, false};
      (void)operand2(cpu, in, &m, &shifted);
      const oemu_exec_shift_result *shifted_ref =
          (in->operand_kind == OEMU_OPERAND_REG_SHIFTED) ? &shifted : NULL;
      const uint64_t n = read_g(cpu, in->rn, false, in->width);
      const bool negated = (in->op == OEMU_OP_BIC) || (in->op == OEMU_OP_BICS) ||
                           (in->op == OEMU_OP_ORN) || (in->op == OEMU_OP_EON);
      if (negated) {
        m = ~m;
      }
      /* If-chain, not a switch: a partial enum switch trips -Wswitch-enum. */
      uint64_t result;
      if ((in->op == OEMU_OP_AND) || (in->op == OEMU_OP_ANDS) || (in->op == OEMU_OP_BIC) ||
          (in->op == OEMU_OP_BICS)) {
        result = n & m;
      } else if ((in->op == OEMU_OP_ORR) || (in->op == OEMU_OP_ORN)) {
        result = n | m;
      } else { /* EOR, EON */
        result = n ^ m;
      }
      write_g(cpu, in->rd, false, in->width, result);
      if ((in->op == OEMU_OP_ANDS) || (in->op == OEMU_OP_BICS)) {
        oemu_regs_set_nzcv(&cpu->regs,
                           logical_flags(oemu_regs_nzcv(&cpu->regs), result, in, shifted_ref));
      }
      break;
    }

    case OEMU_OP_MOVZ:
      /* uimm is the FINAL value: the decoder has already placed the 16-bit
       * field at its hw-selected position (shift_amount documents that
       * placement but must not be applied a second time here). */
      write_g(cpu, in->rd, false, in->width, in->uimm);
      break;
    case OEMU_OP_MOVN:
      write_g(cpu, in->rd, false, in->width, ~in->uimm);
      break;
    case OEMU_OP_MOVK: {
      const unsigned shift = in->shift_amount;
      const uint64_t old = read_g(cpu, in->rd, false, in->width);
      const uint64_t mask = UINT64_C(0xFFFF) << shift;
      write_g(cpu, in->rd, false, in->width, (old & ~mask) | (in->uimm & mask));
      break;
    }

    case OEMU_OP_ADR:
    case OEMU_OP_ADRP:
      write_g(cpu, in->rd, false, in->width, (uint64_t)in->imm);
      break;

    case OEMU_OP_SBFM:
    case OEMU_OP_BFM:
    case OEMU_OP_UBFM:
      st = do_bitfield(cpu, in);
      break;
    case OEMU_OP_EXTR: {
      /* (Rn:Rm) rotated right by the amount, low width taken. */
      const uint64_t lo = read_g(cpu, in->rm, false, in->width);
      const uint64_t hi = read_g(cpu, in->rn, false, in->width);
      const unsigned bits = reg_bits_of(in->width);
      uint64_t result;
      if (in->shift_amount == 0U) {
        result = lo;
      } else {
        const uint64_t concat_lo = lo >> in->shift_amount;
        const uint64_t concat_hi =
            (in->shift_amount == bits) ? hi : (hi << (bits - in->shift_amount));
        result = concat_lo | concat_hi;
      }
      write_g(cpu, in->rd, false, in->width, result);
      break;
    }

    case OEMU_OP_LSLV:
    case OEMU_OP_LSRV:
    case OEMU_OP_ASRV:
    case OEMU_OP_RORV: {
      const uint64_t src = read_g(cpu, in->rn, false, in->width);
      const uint64_t amt_raw = read_g(cpu, in->rm, false, in->width);
      const unsigned bits = reg_bits_of(in->width);
      const unsigned amt = (unsigned)(amt_raw & (bits - 1U));
      /* Variable shifts use their own amount rules: #0 is a plain identity
       * here (unlike the fixed-shift forms), and flags are never touched. */
      uint64_t value;
      if (amt == 0U) {
        const uint64_t mask =
            (bits == 64U) ? ~UINT64_C(0) : ((UINT64_C(1) << bits) - UINT64_C(1));
        value = src & mask;
      } else {
        /* If-chain, not nested ternaries (readability-avoid-nested-conditional). */
        oemu_shift_type type = OEMU_SHIFT_ROR;
        if (in->op == OEMU_OP_LSLV) {
          type = OEMU_SHIFT_LSL;
        } else if (in->op == OEMU_OP_LSRV) {
          type = OEMU_SHIFT_LSR;
        } else if (in->op == OEMU_OP_ASRV) {
          type = OEMU_SHIFT_ASR;
        }
        value = oemu_exec_internal_shift_operand(src, type, amt, in->width).value;
      }
      write_g(cpu, in->rd, false, in->width, value);
      break;
    }

    case OEMU_OP_CRC32: {
      /* Reflected CRC-32 (poly 0x04C11DB7). The data operand is Rm: its low
       * `in->uimm` bytes (all 8 for CRC32X, read from the full 64-bit
       * register); the seed is Rn's low 32 bits; the result is 32-bit. */
      const uint32_t seed = (uint32_t)read_g(cpu, in->rn, false, OEMU_REG_W32);
      const uint64_t data = read_g(cpu, in->rm, false, OEMU_REG_W64);
      write_g(cpu, in->rd, false, OEMU_REG_W32,
              oemu_exec_internal_crc32(seed, data, (unsigned)in->uimm));
      break;
    }
    case OEMU_OP_RBIT:
      write_g(cpu, in->rd, false, in->width,
              oemu_exec_internal_rbit(read_g(cpu, in->rn, false, in->width), in->width));
      break;
    case OEMU_OP_CLZ:
      write_g(cpu, in->rd, false, in->width,
              oemu_exec_internal_clz(read_g(cpu, in->rn, false, in->width), in->width));
      break;
    case OEMU_OP_CLS:
      write_g(cpu, in->rd, false, in->width,
              oemu_exec_internal_cls(read_g(cpu, in->rn, false, in->width), in->width));
      break;
    case OEMU_OP_REV:
      write_g(cpu, in->rd, false, in->width,
              oemu_exec_internal_rev(read_g(cpu, in->rn, false, in->width), in->width));
      break;
    case OEMU_OP_REV16:
      write_g(cpu, in->rd, false, in->width,
              oemu_exec_internal_rev16(read_g(cpu, in->rn, false, in->width), in->width));
      break;
    case OEMU_OP_REV32:
      write_g(cpu, in->rd, false, in->width,
              oemu_exec_internal_rev32(read_g(cpu, in->rn, false, in->width)));
      break;

    case OEMU_OP_UDIV:
    case OEMU_OP_SDIV: {
      /* Division never faults and division by zero never traps: both simply
       * produce zero on AArch64, as does INT_MIN / -1 for the signed form. */
      const uint64_t d = read_g(cpu, in->rm, false, in->width);
      const uint64_t n = read_g(cpu, in->rn, false, in->width);
      uint64_t q = 0U;
      if (d != 0U) {
        if (in->op == OEMU_OP_UDIV) {
          q = (in->width == OEMU_REG_W32) ? ((uint32_t)n / (uint32_t)d) : (n / d);
        } else if (in->width == OEMU_REG_W32) {
          const int32_t nn = (int32_t)(uint32_t)n;
          const int32_t dd = (int32_t)(uint32_t)d;
          q = (uint32_t)((nn == INT32_MIN && dd == -1) ? INT32_MIN : nn / dd);
        } else {
          const int64_t nn = (int64_t)n;
          const int64_t dd = (int64_t)d;
          q = (uint64_t)((nn == INT64_MIN && dd == -1) ? INT64_MIN : nn / dd);
        }
      }
      write_g(cpu, in->rd, false, in->width, q);
      break;
    }

    case OEMU_OP_MADD:
    case OEMU_OP_MSUB: {
      const uint64_t prod =
          read_g(cpu, in->rn, false, in->width) * read_g(cpu, in->rm, false, in->width);
      const uint64_t acc = read_g(cpu, in->ra, false, in->width);
      write_g(cpu, in->rd, false, in->width,
              (in->op == OEMU_OP_MADD) ? (acc + prod) : (acc - prod));
      break;
    }
    case OEMU_OP_SMADDL:
    case OEMU_OP_SMSUBL:
    case OEMU_OP_UMADDL:
    case OEMU_OP_UMSUBL: {
      /* The widening family: two 32-bit multiplicands (sign choice per op),
       * but the ADDEND (Ra) and the destination are full 64-bit. Ra is the
       * third operand added to the widened product; reading it as W32 drops
       * the top half of a 64-bit pointer, so a guest that scales an index by
       * an element size and adds a base address faults on a bogus low
       * address -- Linux's to_desc does exactly `umaddl x0, w2, w1, x0` on the
       * printk ring pointer, which is what stalled the earlycon banner. */
      const bool sgn = (in->op == OEMU_OP_SMADDL) || (in->op == OEMU_OP_SMSUBL);
      const bool add = (in->op == OEMU_OP_SMADDL) || (in->op == OEMU_OP_UMADDL);
      const uint64_t n = read_g(cpu, in->rn, false, OEMU_REG_W32);
      const uint64_t m = read_g(cpu, in->rm, false, OEMU_REG_W32);
      const uint64_t a = read_g(cpu, in->ra, false, OEMU_REG_W64);
      uint64_t prod;
      if (sgn) {
        prod = (uint64_t)((int64_t)(int32_t)(uint32_t)n * (int64_t)(int32_t)(uint32_t)m);
      } else {
        prod = (n & UINT32_MAX) * (m & UINT32_MAX);
      }
      write_g(cpu, in->rd, false, OEMU_REG_W64, add ? (a + prod) : (a - prod));
      break;
    }
    case OEMU_OP_SMULH:
      write_g(cpu, in->rd, false, OEMU_REG_W64,
              oemu_exec_internal_smulh(read_g(cpu, in->rn, false, OEMU_REG_W64),
                                       read_g(cpu, in->rm, false, OEMU_REG_W64)));
      break;
    case OEMU_OP_UMULH:
      write_g(cpu, in->rd, false, OEMU_REG_W64,
              oemu_exec_internal_umulh(read_g(cpu, in->rn, false, OEMU_REG_W64),
                                       read_g(cpu, in->rm, false, OEMU_REG_W64)));
      break;

    case OEMU_OP_CSEL:
    case OEMU_OP_CSINC:
    case OEMU_OP_CSINV:
    case OEMU_OP_CSNEG:
      st = do_csel(cpu, in);
      break;
    case OEMU_OP_CCMP:
    case OEMU_OP_CCMN:
      st = do_ccmp(cpu, in);
      break;

    case OEMU_OP_B:
      take_branch = true;
      branch_target = (uint64_t)in->imm;
      break;
    case OEMU_OP_BL:
      take_branch = true;
      branch_target = (uint64_t)in->imm;
      link = true;
      break;
    case OEMU_OP_B_COND:
      take_branch = oemu_regs_cond_holds(&cpu->regs, in->cond);
      branch_target = (uint64_t)in->imm;
      break;
    case OEMU_OP_BR:
      take_branch = true;
      branch_target = read_g(cpu, in->rn, false, OEMU_REG_W64);
      break;
    case OEMU_OP_BLR:
      take_branch = true;
      link = true;
      branch_target = read_g(cpu, in->rn, false, OEMU_REG_W64);
      break;
    case OEMU_OP_RET:
      take_branch = true;
      branch_target = read_g(cpu, in->rn, false, OEMU_REG_W64);
      break;
    case OEMU_OP_CBZ:
    case OEMU_OP_CBNZ: {
      const uint64_t v = read_g(cpu, in->rd, false, in->width);
      take_branch = ((v == 0U) == (in->op == OEMU_OP_CBZ));
      branch_target = (uint64_t)in->imm;
      break;
    }
    case OEMU_OP_TBZ:
    case OEMU_OP_TBNZ: {
      const uint64_t v = read_g(cpu, in->rd, false, in->width);
      const bool bit = ((v >> in->bit_pos) & UINT64_C(1)) != 0U;
      take_branch = (bit == (in->op == OEMU_OP_TBNZ));
      branch_target = (uint64_t)in->imm;
      break;
    }

    case OEMU_OP_LDR:
    case OEMU_OP_STR:
    case OEMU_OP_LDRS:
    case OEMU_OP_LDAR:
    case OEMU_OP_STLR:
      st = do_single_mem(cpu, mem, in);
      break;
    case OEMU_OP_LDP:
    case OEMU_OP_STP:
      st = in->is_vector ? do_pair_vector(cpu, mem, in) : do_pair(cpu, mem, in);
      break;
    case OEMU_OP_LDPSW:
      st = do_pair(cpu, mem, in);
      break;
    case OEMU_OP_LDXR:
    case OEMU_OP_STXR:
      st = do_exclusive(cpu, mem, in);
      break;

    case OEMU_OP_SVC:
      st = do_svc(cpu, env, mem);
      break;
    case OEMU_OP_HVC:
    case OEMU_OP_SMC:
    case OEMU_OP_ERET:
    case OEMU_OP_SYS:
      /* EL1+ operations: real instructions the user-mode subset refuses, the
       * same signal an unimplemented MRS/MSR row gives. System-mode handling
       * of these lands with the vCPU (M2c) and the MMU (M3) for SYS. */
      st = OEMU_ERR_UNSUPPORTED;
      break;
    case OEMU_OP_BRK:
    case OEMU_OP_HLT:
      /* A guest-initiated trap stops the run exactly like a fault. */
      st = OEMU_ERR_FAULT;
      break;
    case OEMU_OP_NOP:
    case OEMU_OP_HINT:
    case OEMU_OP_WFI:
    case OEMU_OP_WFE:
    case OEMU_OP_BARRIER:
      break; /* architecturally observable: nothing happens */
    case OEMU_OP_MRS:
      /* The user-mode path has no sysreg state to consult -- no oemu_sysregs at
       * all -- so it hands down a null table and keeps the old refusal for every
       * selector outside its five. */
      st = do_mrs(cpu, NULL, in);
      break;
    case OEMU_OP_MSR:
      st = do_msr(cpu, NULL, in);
      break;
    case OEMU_OP_MSR_IMM:
      /* The user-mode subset does not touch privileged mode bits; system mode
       * intercepts this before the shared switch. */
      st = OEMU_ERR_UNSUPPORTED;
      break;

    default:
      st = OEMU_ERR_UNSUPPORTED; /* a newer decoder cannot outdate this switch */
      break;
  }

  if (st != OEMU_OK) {
    return st;
  }
  if (take_branch) {
    if (link) {
      oemu_regs_write(&cpu->regs, 30U, OEMU_REG_W64, cpu->regs.pc + OEMU_INSN_SIZE);
    }
    oemu_regs_set_pc(&cpu->regs, branch_target);
  } else {
    oemu_regs_advance_pc(&cpu->regs);
  }
  return OEMU_OK;
}

oemu_status oemu_exec_internal_dispatch(oemu_cpu *cpu, oemu_memory *mem, oemu_sysenv *env,
                                        const oemu_insn *in) {
  if (cpu == NULL || mem == NULL || in == NULL || in->op == OEMU_OP_UNKNOWN) {
    return OEMU_ERR_INVALID_ARG;
  }
  const oemu_memops bus = oemu_memory_memops(mem);
  const oemu_env_ops environment = oemu_sysenv_envops(env);
  return oemu_exec_internal_dispatch_bus(cpu, &bus, (env != NULL) ? &environment : NULL, in);
}

oemu_status oemu_cpu_init(oemu_cpu *cpu, uint64_t entry_pc, uint64_t initial_sp) {
  if (cpu == NULL) {
    return OEMU_ERR_INVALID_ARG;
  }
  const oemu_status st = oemu_regs_init(&cpu->regs, entry_pc, initial_sp);
  if (st != OEMU_OK) {
    return st;
  }
  cpu->monitor_addr = 0U;
  cpu->monitor_size = 0U;
  cpu->monitor_valid = false;
  cpu->tpidrur_el0 = 0U;
  for (unsigned i = 0U; i < 32U; i++) {
    cpu->v[i][0] = 0U;
    cpu->v[i][1] = 0U;
  }
  return OEMU_OK;
}

oemu_status oemu_exec_step_bus(oemu_cpu *cpu, const oemu_memops *mem, const oemu_env_ops *env,
                               oemu_insn *insn_out) {
  if (cpu == NULL || mem == NULL) {
    return OEMU_ERR_INVALID_ARG;
  }
  uint32_t word = 0U;
  const oemu_status fetch = mem->fetch32(mem->ctx, oemu_regs_pc(&cpu->regs), &word);
  if (fetch != OEMU_OK) {
    return fetch;
  }
  oemu_insn insn;
  const oemu_status dec = oemu_decode(word, oemu_regs_pc(&cpu->regs), &insn);
  if (insn_out != NULL) {
    *insn_out = insn; /* zeroed by the decoder on failure: safe to expose */
  }
  if (dec != OEMU_OK) {
    return dec;
  }
  return oemu_exec_internal_dispatch_bus(cpu, mem, env, &insn);
}

/* Whether the environment says the guest has stopped. A missing environment
 * or a missing callback is "still running", the same answer the concrete
 * oemu_sysenv gives for a NULL pointer. */
static bool envops_halted(const oemu_env_ops *env) {
  return (env != NULL) && (env->halted != NULL) && env->halted(env->ctx);
}

oemu_status oemu_exec_run_bus(oemu_cpu *cpu, const oemu_memops *mem, const oemu_env_ops *env,
                              uint64_t max_insns, uint64_t *completed_out) {
  uint64_t done = 0U;
  while (done < max_insns) {
    const oemu_status st = oemu_exec_step_bus(cpu, mem, env, NULL);
    if (st != OEMU_OK) {
      if (completed_out != NULL) {
        *completed_out = done;
      }
      return st;
    }
    done++;
    if (envops_halted(env)) {
      break;
    }
  }
  if (completed_out != NULL) {
    *completed_out = done;
  }
  return envops_halted(env) ? OEMU_OK : OEMU_ERR_TIMEOUT;
}

/* --- concrete-bus wrappers ------------------------------------------------------ */

/*
 * The old entry points are now convenience over the seam: they build a view
 * over a concrete oemu_memory/oemu_sysenv pair and hand it to the bus
 * version. Building a view allocates nothing and costs a handful of pointer
 * copies, which is nothing next to the instruction it drives. A NULL
 * environment stays NULL so an SVC keeps reporting OEMU_ERR_UNSUPPORTED
 * rather than reaching a half-valid envops.
 */
oemu_status oemu_exec_step(oemu_cpu *cpu, oemu_memory *mem, oemu_sysenv *env,
                           oemu_insn *insn_out) {
  if (cpu == NULL || mem == NULL) {
    return OEMU_ERR_INVALID_ARG;
  }
  const oemu_memops bus = oemu_memory_memops(mem);
  const oemu_env_ops environment = oemu_sysenv_envops(env);
  return oemu_exec_step_bus(cpu, &bus, (env != NULL) ? &environment : NULL, insn_out);
}

oemu_status oemu_exec_run(oemu_cpu *cpu, oemu_memory *mem, oemu_sysenv *env, uint64_t max_insns,
                          uint64_t *completed_out) {
  const oemu_memops bus = oemu_memory_memops(mem);
  const oemu_env_ops environment = oemu_sysenv_envops(env);
  return oemu_exec_run_bus(cpu, &bus, (env != NULL) ? &environment : NULL, max_insns,
                           completed_out);
}

oemu_status oemu_vec_read(const oemu_cpu *cpu, unsigned n, uint64_t *lo, uint64_t *hi) {
  OEMU_REQUIRE(cpu != NULL, "NULL cpu in oemu_vec_read");
  if ((lo == NULL) || (hi == NULL)) {
    return OEMU_ERR_INVALID_ARG;
  }
  if (n >= OEMU_VEC_REGS) {
    return OEMU_ERR_INVALID_ARG; /* there is no register 32 to read */
  }
  *lo = cpu->v[n][0];
  *hi = cpu->v[n][1];
  return OEMU_OK;
}

oemu_status oemu_vec_write(oemu_cpu *cpu, unsigned n, uint64_t lo, uint64_t hi) {
  OEMU_REQUIRE(cpu != NULL, "NULL cpu in oemu_vec_write");
  if (n >= OEMU_VEC_REGS) {
    return OEMU_ERR_INVALID_ARG;
  }
  cpu->v[n][0] = lo;
  cpu->v[n][1] = hi;
  return OEMU_OK;
}
