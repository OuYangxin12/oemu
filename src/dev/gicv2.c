/*
 * ARM Generic Interrupt Controller, version 2 -- see include/oemu/gicv2.h.
 *
 * Modelled from qemu-system-aarch64's `arm_gic` (hw/intc/arm_gic.c) and the
 * driver drivers/irqchip/irq-gic.c: a distributor and one CPU interface, each
 * a 64 KiB window, a GICD_PIDR2 == 0x2B identity the driver insists on, and a
 * priority scan that acknowledges the highest-precedence eligible line. The
 * driver touches the distributor during init_IRQ (disable, program targets,
 * config, group, re-enable) and the CPU interface on every handle_irq, so
 * every offset in both windows must answer: modelled registers for real, the
 * rest read-as-zero / write-ignore -- never a fault, because a Data Abort in
 * gic_of_init is exactly what killed the boot before this device existed.
 */

#include "oemu/gicv2.h"

#include "oemu/check.h"

#include <string.h>

#include "gicv2_internal.h"

/* Distributor register offsets (GICv2, fixed by the architecture). */
#define DIST_CTL        0x000U
#define DIST_TYPER      0x004U
#define DIST_IIDR       0x008U
#define DIST_IGROUP0    0x080U
#define DIST_ISENABLER0 0x100U
#define DIST_ICENABLER0 0x180U
#define DIST_ISPENDR0   0x200U
#define DIST_ICPENDR0   0x280U
#define DIST_ISACTIVER0 0x300U
#define DIST_ICACTIVER0 0x380U
#define DIST_PRIORITY0  0x400U
#define DIST_TARGET0    0x800U
#define DIST_CFG0       0xC00U
#define DIST_SGIR       0xF00U
#define DIST_CID0       0xF88U
#define DIST_PID0       0xFE0U

/* CPU interface register offsets. */
#define CPU_CTL  0x000U
#define CPU_PMR  0x004U
#define CPU_BPR  0x008U
#define CPU_IAR  0x00CU
#define CPU_EOIR 0x010U

/* PrimeCell / peripheral identity a GIC-400 answers with. */
#define CIDR0 0x0DU
#define CIDR1 0xF0U
#define CIDR2 0x05U
#define CIDR3 0xB1U
#define PIDR0 0x00U
#define PIDR1 0x00U
#define PIDR2 0x2BU /* the value the driver matches on to accept a GICv2 */
#define PIDR3 0x02U

static oemu_status dist_read(void *ctx, uint64_t offset, oemu_mem_size size,
                             uint64_t *value_out);
static oemu_status dist_write(void *ctx, uint64_t offset, oemu_mem_size size, uint64_t value);
static oemu_status cpu_read(void *ctx, uint64_t offset, oemu_mem_size size,
                            uint64_t *value_out);
static oemu_status cpu_write(void *ctx, uint64_t offset, oemu_mem_size size, uint64_t value);

/* The modelled line count, derived back from GICD_TYPER.itlines: itlines is
 * the group count minus one, and each group is 32 lines, so the bank holds
 * ((itlines + 1) * 32) interrupts. This is the scan loop's bound, so it must
 * be the true line count -- returning the group count here would make the scan
 * stop after two lines and never acknowledge anything past id 1. */
static unsigned lines_of(const oemu_gicv2 *gic) {
  return ((unsigned)(gic->typer & 0x1FU) + 1U) * 32U;
}

/* Running priority = the value of the highest-precedence *active* line (the
 * lowest priority byte), or 256 when nothing is active. A pending line may
 * only be acknowledged when its priority beats this floor. */
static uint32_t gic_running_priority(const oemu_gicv2 *gic) {
  uint32_t best = 256U;
  for (unsigned i = 0U; i < OEMU_GICV2_LINES; ++i) {
    if ((gic->active[i] != 0U) && ((uint32_t)gic->priority[i] < best)) {
      best = (uint32_t)gic->priority[i];
    }
  }
  return best;
}

unsigned oemu_gicv2_internal_scan(const uint8_t enable[OEMU_GICV2_LINES],
                                  const uint8_t pending[OEMU_GICV2_LINES],
                                  const uint8_t active[OEMU_GICV2_LINES],
                                  const uint8_t priority[OEMU_GICV2_LINES],
                                  const uint8_t target[OEMU_GICV2_LINES], uint32_t running_pri,
                                  uint32_t pmr, unsigned lines) {
  /* Lower priority *value* == higher precedence. Take the pending, enabled,
   * non-active, targeted line with the lowest value that still clears the
   * mask (value < pmr) and the running floor (value < running_pri); ties go
   * to the lower interrupt id, which the "pri >= best_pri" skip does by only
   * replacing on a strictly lower value. */
  unsigned best = OEMU_GICV2_SPURIOUS;
  uint32_t best_pri = running_pri;
  for (unsigned i = 0U; (i < lines) && (i < OEMU_GICV2_LINES); ++i) {
    if ((enable[i] == 0U) || (pending[i] == 0U) || (active[i] != 0U) ||
        ((target[i] & 1U) == 0U)) {
      continue;
    }
    const uint32_t pri = (uint32_t)priority[i];
    if ((pri >= pmr) || (pri >= best_pri)) {
      continue;
    }
    best_pri = pri;
    best = i;
  }
  return best;
}

static unsigned gic_acknowledge(oemu_gicv2 *gic) {
  const uint32_t running = gic_running_priority(gic);
  const unsigned id =
      oemu_gicv2_internal_scan(gic->enable, gic->pending, gic->active, gic->priority,
                               gic->target, running, (gic->cpu_pmr & 0xFFU), lines_of(gic));
  if (id == OEMU_GICV2_SPURIOUS) {
    return OEMU_GICV2_SPURIOUS;
  }
  gic->active[id] = 1U;
  gic->pending[id] = 0U; /* the acknowledgement consumes the latched pending */
  gic->running_pri = gic_running_priority(gic);
  return id;
}

void oemu_gicv2_init(oemu_gicv2 *gic, unsigned lines) {
  OEMU_REQUIRE(gic != NULL, "NULL oemu_gicv2");
  OEMU_REQUIRE(((lines % 32U) == 0U) && (lines >= 32U) && (lines <= OEMU_GICV2_LINES),
               "gicv2 lines must be a multiple of 32 within the modelled bank");
  memset(gic, 0, sizeof(*gic));
  gic->dist_ops.ctx = gic;
  gic->dist_ops.read = &dist_read;
  gic->dist_ops.write = &dist_write;
  gic->cpu_ops.ctx = gic;
  gic->cpu_ops.read = &cpu_read;
  gic->cpu_ops.write = &cpu_write;
  /* GICD_TYPER: nclusters=0 (bits[10:8]=0 so the CPU count is read from
   * bits[23:16]), ncpu=16 (bits[23:16]: the field is log2(CPU interface
   * count)-1 and 16 -> "up to 8"), itlines = lines/32 - 1 (bits[4:0]). */
  gic->typer = ((uint32_t)16U << 16) | (uint32_t)((lines / 32U) - 1U);
  gic->iidr = 0U;
  /* Every line starts disabled with a middling priority, as the real reset.
   * SGI (0-15) and PPI (16-31) targets are architecturally fixed to the
   * single CPU, so they start addressed here (bit0 set) and the driver never
   * rewrites them; only SPI (32+) targets are programmable and start clear. */
  for (unsigned i = 0U; i < OEMU_GICV2_LINES; ++i) {
    gic->priority[i] = 0x80U;
    gic->target[i] = (i < 32U) ? 1U : 0U;
  }
}

/* A banked [0x100..0x380] register selects one 32-line word; the low bits
 * of the offset give the line base. Returns the packed 32-bit state word. */
static uint32_t pack_bank(const uint8_t *state, unsigned base_line) {
  uint32_t v = 0U;
  for (unsigned b = 0U; b < 32U; ++b) {
    if ((base_line + b) < OEMU_GICV2_LINES && (state[base_line + b] != 0U)) {
      v |= (UINT32_C(1) << b);
    }
  }
  return v;
}

/* Write an ISENABLER / ICENABLER / ISPENDR / ICPENDR / ISACTIVER / ICACTIVER
 * bank word: `set` names lines whose state is forced to 1 (set) or 0 (clear);
 * lines not named are untouched. */
static void poke_bank(uint8_t *state, unsigned base_line, uint32_t mask, bool set) {
  for (unsigned b = 0U; b < 32U; ++b) {
    if (((mask >> b) & 1U) != 0U) {
      const unsigned line = base_line + b;
      if (line < OEMU_GICV2_LINES) {
        state[line] = set ? 1U : 0U;
      }
    }
  }
}

static oemu_status dist_read(void *ctx, uint64_t offset, oemu_mem_size size,
                             uint64_t *value_out) {
  oemu_gicv2 *gic = (oemu_gicv2 *)ctx;
  OEMU_REQUIRE((gic != NULL) && (value_out != NULL), "NULL gicv2 distributor read");
  (void)size;
  uint32_t v = 0U;

  if ((offset >= DIST_IGROUP0) && (offset < (DIST_IGROUP0 + 0x20U))) {
    const unsigned word = (unsigned)((offset - DIST_IGROUP0) / 4U);
    v = pack_bank(gic->group, word * 32U);
  } else if ((offset >= DIST_ISENABLER0) && (offset < (DIST_ISENABLER0 + 0x80U))) {
    const unsigned word = (unsigned)((offset - DIST_ISENABLER0) / 4U);
    v = pack_bank(gic->enable, word * 32U);
  } else if ((offset >= DIST_ICENABLER0) && (offset < (DIST_ICENABLER0 + 0x80U))) {
    const unsigned word = (unsigned)((offset - DIST_ICENABLER0) / 4U);
    v = pack_bank(gic->enable, word * 32U); /* I*C read the same state as I*E */
  } else if ((offset >= DIST_ISPENDR0) && (offset < (DIST_ISPENDR0 + 0x80U))) {
    const unsigned word = (unsigned)((offset - DIST_ISPENDR0) / 4U);
    v = pack_bank(gic->pending, word * 32U);
  } else if ((offset >= DIST_ICPENDR0) && (offset < (DIST_ICPENDR0 + 0x80U))) {
    const unsigned word = (unsigned)((offset - DIST_ICPENDR0) / 4U);
    v = pack_bank(gic->pending, word * 32U);
  } else if ((offset >= DIST_ISACTIVER0) && (offset < (DIST_ISACTIVER0 + 0x80U))) {
    const unsigned word = (unsigned)((offset - DIST_ISACTIVER0) / 4U);
    v = pack_bank(gic->active, word * 32U);
  } else if ((offset >= DIST_ICACTIVER0) && (offset < (DIST_ICACTIVER0 + 0x80U))) {
    const unsigned word = (unsigned)((offset - DIST_ICACTIVER0) / 4U);
    v = pack_bank(gic->active, word * 32U);
  } else if ((offset >= DIST_PRIORITY0) && (offset < (DIST_PRIORITY0 + 0x100U))) {
    const unsigned word = (unsigned)((offset - DIST_PRIORITY0) / 4U);
    for (unsigned b = 0U; b < 4U; ++b) {
      const unsigned line = word * 4U + b;
      v |= (uint32_t)((line < OEMU_GICV2_LINES) ? gic->priority[line] : 0U) << (b * 8U);
    }
  } else if ((offset >= DIST_TARGET0) && (offset < (DIST_TARGET0 + 0x100U))) {
    const unsigned word = (unsigned)((offset - DIST_TARGET0) / 4U);
    for (unsigned b = 0U; b < 4U; ++b) {
      const unsigned line = word * 4U + b;
      v |= (uint32_t)((line < OEMU_GICV2_LINES) ? gic->target[line] : 0U) << (b * 8U);
    }
  } else if ((offset >= DIST_CFG0) && (offset < (DIST_CFG0 + 0x40U))) {
    const unsigned word = (unsigned)((offset - DIST_CFG0) / 4U);
    for (unsigned b = 0U; b < 16U; ++b) {
      const unsigned line = word * 16U + b;
      if (line < OEMU_GICV2_LINES) {
        v |= (uint32_t)gic->config[line] << (b * 2U);
      }
    }
  } else {
    switch ((unsigned)(offset & ~UINT64_C(3))) {
      case DIST_CTL:
        v = gic->ctl;
        break;
      case DIST_TYPER:
        v = gic->typer;
        break;
      case DIST_IIDR:
        v = gic->iidr;
        break;
      case DIST_CID0:
        v = CIDR0;
        break;
      case DIST_CID0 + 0x4U:
        v = CIDR1;
        break;
      case DIST_CID0 + 0x8U:
        v = CIDR2;
        break;
      case DIST_CID0 + 0xCU:
        v = CIDR3;
        break;
      case DIST_PID0:
        v = PIDR0;
        break;
      case DIST_PID0 + 0x4U:
        v = PIDR1;
        break;
      case DIST_PID0 + 0x8U:
        v = PIDR2; /* the 0x2B the driver matches on */
        break;
      case DIST_PID0 + 0xCU:
        v = PIDR3;
        break;
      default:
        v = 0U; /* RES0 anywhere else -- never a fault */
        break;
    }
  }
  *value_out = v;
  return OEMU_OK;
}

static oemu_status dist_write(void *ctx, uint64_t offset, oemu_mem_size size, uint64_t value) {
  oemu_gicv2 *gic = (oemu_gicv2 *)ctx;
  OEMU_REQUIRE(gic != NULL, "NULL gicv2 distributor write");
  (void)size;
  const uint32_t v = (uint32_t)value;

  if ((offset >= DIST_IGROUP0) && (offset < (DIST_IGROUP0 + 0x20U))) {
    const unsigned word = (unsigned)((offset - DIST_IGROUP0) / 4U);
    const unsigned base = word * 32U;
    for (unsigned b = 0U; b < 32U; ++b) {
      if ((base + b) < OEMU_GICV2_LINES) {
        gic->group[base + b] = (uint8_t)((v >> b) & 1U);
      }
    }
  } else if ((offset >= DIST_ISENABLER0) && (offset < (DIST_ISENABLER0 + 0x80U))) {
    poke_bank(gic->enable, ((unsigned)((offset - DIST_ISENABLER0) / 4U)) * 32U, v, true);
  } else if ((offset >= DIST_ICENABLER0) && (offset < (DIST_ICENABLER0 + 0x80U))) {
    poke_bank(gic->enable, ((unsigned)((offset - DIST_ICENABLER0) / 4U)) * 32U, v, false);
  } else if ((offset >= DIST_ISPENDR0) && (offset < (DIST_ISPENDR0 + 0x80U))) {
    poke_bank(gic->pending, ((unsigned)((offset - DIST_ISPENDR0) / 4U)) * 32U, v, true);
  } else if ((offset >= DIST_ICPENDR0) && (offset < (DIST_ICPENDR0 + 0x80U))) {
    poke_bank(gic->pending, ((unsigned)((offset - DIST_ICPENDR0) / 4U)) * 32U, v, false);
  } else if ((offset >= DIST_ISACTIVER0) && (offset < (DIST_ISACTIVER0 + 0x80U))) {
    poke_bank(gic->active, ((unsigned)((offset - DIST_ISACTIVER0) / 4U)) * 32U, v, true);
  } else if ((offset >= DIST_ICACTIVER0) && (offset < (DIST_ICACTIVER0 + 0x80U))) {
    poke_bank(gic->active, ((unsigned)((offset - DIST_ICACTIVER0) / 4U)) * 32U, v, false);
  } else if ((offset >= DIST_PRIORITY0) && (offset < (DIST_PRIORITY0 + 0x100U))) {
    const unsigned word = (unsigned)((offset - DIST_PRIORITY0) / 4U);
    for (unsigned b = 0U; b < 4U; ++b) {
      const unsigned line = word * 4U + b;
      if (line < OEMU_GICV2_LINES) {
        gic->priority[line] = (uint8_t)((v >> (b * 8U)) & 0xFFU);
      }
    }
  } else if ((offset >= DIST_TARGET0) && (offset < (DIST_TARGET0 + 0x100U))) {
    const unsigned word = (unsigned)((offset - DIST_TARGET0) / 4U);
    for (unsigned b = 0U; b < 4U; ++b) {
      const unsigned line = word * 4U + b;
      /* Only SPI (>= 32) targets are writable; SGI/PPI targets are fixed. */
      if ((line < OEMU_GICV2_LINES) && (line >= 32U)) {
        gic->target[line] = (uint8_t)((v >> (b * 8U)) & 0xFFU);
      }
    }
  } else if ((offset >= DIST_CFG0) && (offset < (DIST_CFG0 + 0x40U))) {
    const unsigned word = (unsigned)((offset - DIST_CFG0) / 4U);
    for (unsigned b = 0U; b < 16U; ++b) {
      const unsigned line = word * 16U + b;
      if (line < OEMU_GICV2_LINES) {
        gic->config[line] = (uint8_t)((v >> (b * 2U)) & 0x3U);
      }
    }
  } else {
    switch ((unsigned)(offset & ~UINT64_C(3))) {
      case DIST_CTL:
        gic->ctl = v & 0x3FU;
        break;
      case DIST_SGIR:
        gic->sgir = v; /* SGI software trigger: single-CPU, nothing to route */
        break;
      default:
        break; /* RAZ/WI elsewhere -- never a fault */
    }
  }
  return OEMU_OK;
}

static oemu_status cpu_read(void *ctx, uint64_t offset, oemu_mem_size size,
                            uint64_t *value_out) {
  oemu_gicv2 *gic = (oemu_gicv2 *)ctx;
  OEMU_REQUIRE((gic != NULL) && (value_out != NULL), "NULL gicv2 cpu interface read");
  (void)size;
  uint32_t v = 0U;
  switch ((unsigned)(offset & ~UINT64_C(3))) {
    case CPU_CTL:
      v = gic->cpu_ctl;
      break;
    case CPU_PMR:
      v = gic->cpu_pmr;
      break;
    case CPU_BPR:
      v = gic->cpu_bpr;
      break;
    case CPU_IAR:
      v = gic_acknowledge(gic); /* the ack: highest-precedence eligible, or 1023 */
      break;
    default:
      v = 0U;
      break;
  }
  *value_out = v;
  return OEMU_OK;
}

static oemu_status cpu_write(void *ctx, uint64_t offset, oemu_mem_size size, uint64_t value) {
  oemu_gicv2 *gic = (oemu_gicv2 *)ctx;
  OEMU_REQUIRE(gic != NULL, "NULL gicv2 cpu interface write");
  (void)size;
  switch ((unsigned)(offset & ~UINT64_C(3))) {
    case CPU_CTL:
      gic->cpu_ctl = (uint32_t)value & 1U;
      break;
    case CPU_PMR:
      gic->cpu_pmr = (uint32_t)value & 0xFFU;
      break;
    case CPU_BPR:
      gic->cpu_bpr = (uint32_t)value & 0x7U;
      break;
    case CPU_EOIR: {
      const unsigned id = (unsigned)((uint32_t)value & 0x3FFU);
      if (id < OEMU_GICV2_LINES) {
        gic->active[id] = 0U;
      }
      gic->running_pri = gic_running_priority(gic);
      break;
    }
    default:
      break;
  }
  return OEMU_OK;
}

int oemu_gicv2_irq_level(const oemu_gicv2 *gic) {
  OEMU_REQUIRE(gic != NULL, "NULL oemu_gicv2");
  if (((gic->ctl & 1U) == 0U) || ((gic->cpu_ctl & 1U) == 0U)) {
    return 0;
  }
  const uint32_t running = gic_running_priority(gic);
  const unsigned id =
      oemu_gicv2_internal_scan(gic->enable, gic->pending, gic->active, gic->priority,
                               gic->target, running, (gic->cpu_pmr & 0xFFU), lines_of(gic));
  return (id != OEMU_GICV2_SPURIOUS) ? 1 : 0;
}

unsigned oemu_gicv2_nr_lines(const oemu_gicv2 *gic) {
  OEMU_REQUIRE(gic != NULL, "NULL oemu_gicv2");
  return lines_of(gic);
}

void oemu_gicv2_set_pending(oemu_gicv2 *gic, unsigned intid, bool level) {
  OEMU_REQUIRE(gic != NULL, "NULL oemu_gicv2");
  if (intid >= OEMU_GICV2_LINES) {
    return;
  }
  gic->pending[intid] = level ? 1U : 0U;
}
