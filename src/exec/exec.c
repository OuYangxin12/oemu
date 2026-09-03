/*
 * A64 interpreter core. See include/oemu/exec.h for the contract and
 * src/exec/exec_internal.h for what each helper means.
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

static void write_g(oemu_cpu *cpu, unsigned n, bool sp_form, oemu_reg_width width,
                    uint64_t value) {
  if (sp_form) {
    oemu_regs_write_sp_form(&cpu->regs, n, width, value);
  } else {
    oemu_regs_write(&cpu->regs, n, width, value);
  }
}

/*
 * After oemu_memory_validate succeeded, an access with the same arguments
 * cannot fail -- the model never allocates or re-shapes between calls. A
 * failure here is therefore a bug in oemu, not a guest event, and is reported
 * as one.
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

static oemu_status do_mrs(oemu_cpu *cpu, const oemu_insn *in) {
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
      /* CurrentEL and every other register: at EL0 there is nothing honest to
       * return except a refusal. */
      return OEMU_ERR_UNSUPPORTED;
  }
  write_g(cpu, in->rd, false, OEMU_REG_W64, value);
  return OEMU_OK;
}

static oemu_status do_msr(oemu_cpu *cpu, const oemu_insn *in) {
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
    default: /* TPIDRRO_EL0 is read-only; the rest are outside the subset */
      return OEMU_ERR_UNSUPPORTED;
  }
  return OEMU_OK;
}

static oemu_status do_svc(oemu_cpu *cpu, oemu_sysenv *env, oemu_memory *mem) {
  if (env == NULL) {
    return OEMU_ERR_UNSUPPORTED;
  }
  uint64_t args[6];
  for (unsigned i = 0U; i < 6U; i++) {
    args[i] = oemu_regs_read(&cpu->regs, i, OEMU_REG_W64);
  }
  const uint64_t nr = oemu_regs_read(&cpu->regs, 8U, OEMU_REG_W64);
  const int64_t result = oemu_sysenv_syscall(env, mem, nr, args);
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

static oemu_status do_pair(oemu_cpu *cpu, oemu_memory *mem, const oemu_insn *in) {
  uint64_t addr;
  uint64_t writeback;
  (void)resolve_mem_addr(cpu, in, &addr, &writeback);
  const uint64_t addr2 = addr + pair_transfer(in);
  const uint64_t transfer = pair_transfer(in);

  /* Validate both transfers before touching anything, so a fault on the
   * second one cannot commit the first. */
  oemu_status st = oemu_memory_validate(
      mem, addr, transfer, (in->op == OEMU_OP_STP) ? OEMU_PERM_WRITE : OEMU_PERM_READ);
  if (st == OEMU_OK) {
    st = oemu_memory_validate(mem, addr2, transfer,
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
    access_or_panic(oemu_memory_write(mem, addr, in->mem_size, v1));
    access_or_panic(oemu_memory_write(mem, addr2, in->mem_size, v2));
    note_store(cpu, addr, transfer * 2U);
  } else {
    access_or_panic(oemu_memory_read(mem, addr, in->mem_size, in->is_signed_load, &v1));
    access_or_panic(oemu_memory_read(mem, addr2, in->mem_size, in->is_signed_load, &v2));
    write_g(cpu, in->rd, false, in->width, v1);
    write_g(cpu, in->rt2, false, in->width, v2);
  }
  if (in->index_mode != OEMU_INDEX_NONE) {
    write_g(cpu, in->rn, true, OEMU_REG_W64, writeback);
  }
  return OEMU_OK;
}

static oemu_status do_single_mem(oemu_cpu *cpu, oemu_memory *mem, const oemu_insn *in) {
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
      oemu_memory_validate(mem, addr, nbytes, is_store ? OEMU_PERM_WRITE : OEMU_PERM_READ);
  if (st != OEMU_OK) {
    return OEMU_ERR_FAULT;
  }

  uint64_t value = 0U;
  if (is_store) {
    value = read_g(cpu, in->rd, false, in->width);
    access_or_panic(oemu_memory_write(mem, addr, size, value));
    note_store(cpu, addr, nbytes);
  } else {
    access_or_panic(oemu_memory_read(mem, addr, size, in->is_signed_load, &value));
    write_g(cpu, in->rd, false, in->width, value);
  }
  if (has_writeback) {
    write_g(cpu, in->rn, true, OEMU_REG_W64, writeback);
  }
  return OEMU_OK;
}

static oemu_status do_exclusive(oemu_cpu *cpu, oemu_memory *mem, const oemu_insn *in) {
  const uint64_t nbytes = UINT64_C(1) << (unsigned)in->mem_size;
  const bool is_load = (in->op == OEMU_OP_LDXR);
  const uint64_t addr = read_g(cpu, in->rn, true, OEMU_REG_W64);

  const oemu_status st =
      oemu_memory_validate(mem, addr, nbytes, is_load ? OEMU_PERM_READ : OEMU_PERM_WRITE);
  if (st != OEMU_OK) {
    return OEMU_ERR_FAULT;
  }

  if (is_load) {
    uint64_t value = 0U;
    access_or_panic(oemu_memory_read(mem, addr, in->mem_size, false, &value));
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
    access_or_panic(oemu_memory_write(mem, addr, in->mem_size, value));
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
   * a wrapped range (immR > immS) shortens len instead of erroring. */
  const unsigned len = (msb < lsb) ? (bits - lsb + msb) : (msb - lsb + 1U);
  uint64_t rot;
  if (lsb == 0U) {
    rot = src;
  } else {
    rot = (src >> lsb) | (src << (bits - lsb));
    rot &= width_mask;
  }
  const uint64_t mask = (len == bits) ? width_mask : ((UINT64_C(1) << len) - UINT64_C(1));
  uint64_t field = rot & mask;

  if (in->op == OEMU_OP_BFM) {
    /* BFM with a wrapped range is the reserved encoding: behave as a no-op. */
    if (msb < lsb) {
      return OEMU_OK;
    }
    const uint64_t old = read_g(cpu, in->rd, false, in->width);
    field = (old & ~((mask << lsb) & width_mask)) | ((field << lsb) & width_mask);
  } else if ((in->op == OEMU_OP_SBFM) && (((field >> (len - 1U)) & UINT64_C(1)) != 0U)) {
    /* Sign-extend inside the register width, then let write_g truncate. */
    field |= width_mask & ~mask;
  }
  write_g(cpu, in->rd, false, in->width, field);
  return OEMU_OK;
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

/* --- the switch ---------------------------------------------------------------- */

oemu_status oemu_exec_internal_dispatch(oemu_cpu *cpu, oemu_memory *mem, oemu_sysenv *env,
                                        const oemu_insn *in) {
  if (cpu == NULL || mem == NULL || in == NULL || in->op == OEMU_OP_UNKNOWN) {
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
      /* The widening family: three 32-bit sources (sign choice per op), one
       * 64-bit destination. */
      const bool sgn = (in->op == OEMU_OP_SMADDL) || (in->op == OEMU_OP_SMSUBL);
      const uint64_t n = read_g(cpu, in->rn, false, OEMU_REG_W32);
      const uint64_t m = read_g(cpu, in->rm, false, OEMU_REG_W32);
      const uint64_t a = read_g(cpu, in->ra, false, OEMU_REG_W32);
      uint64_t prod;
      if (sgn) {
        prod = (uint64_t)((int64_t)(int32_t)(uint32_t)n * (int64_t)(int32_t)(uint32_t)m);
      } else {
        prod = (n & UINT32_MAX) * (m & UINT32_MAX);
      }
      const uint64_t acc = sgn ? (uint64_t)(int64_t)(int32_t)(uint32_t)a : (a & UINT32_MAX);
      write_g(cpu, in->rd, false, OEMU_REG_W64,
              ((in->op == OEMU_OP_SMADDL) || (in->op == OEMU_OP_UMADDL)) ? (acc + prod)
                                                                         : (acc - prod));
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
    case OEMU_OP_BRK:
    case OEMU_OP_HLT:
      /* A guest-initiated trap stops the run exactly like a fault. */
      st = OEMU_ERR_FAULT;
      break;
    case OEMU_OP_NOP:
    case OEMU_OP_HINT:
    case OEMU_OP_BARRIER:
      break; /* architecturally observable: nothing happens */
    case OEMU_OP_MRS:
      st = do_mrs(cpu, in);
      break;
    case OEMU_OP_MSR:
      st = do_msr(cpu, in);
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
  return OEMU_OK;
}

oemu_status oemu_exec_step(oemu_cpu *cpu, oemu_memory *mem, oemu_sysenv *env,
                           oemu_insn *insn_out) {
  if (cpu == NULL || mem == NULL) {
    return OEMU_ERR_INVALID_ARG;
  }
  uint32_t word = 0U;
  const oemu_status fetch = oemu_memory_fetch32(mem, oemu_regs_pc(&cpu->regs), &word);
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
  return oemu_exec_internal_dispatch(cpu, mem, env, &insn);
}

oemu_status oemu_exec_run(oemu_cpu *cpu, oemu_memory *mem, oemu_sysenv *env, uint64_t max_insns,
                          uint64_t *completed_out) {
  uint64_t done = 0U;
  while (done < max_insns) {
    const oemu_status st = oemu_exec_step(cpu, mem, env, NULL);
    if (st != OEMU_OK) {
      if (completed_out != NULL) {
        *completed_out = done;
      }
      return st;
    }
    done++;
    if (oemu_sysenv_exited(env)) {
      break;
    }
  }
  if (completed_out != NULL) {
    *completed_out = done;
  }
  return oemu_sysenv_exited(env) ? OEMU_OK : OEMU_ERR_TIMEOUT;
}
