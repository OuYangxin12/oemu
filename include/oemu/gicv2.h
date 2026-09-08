/*
 * ARM Generic Interrupt Controller, version 2 -- the interrupt source of
 * every arm64 virt boot.
 *
 * A device is a struct, not an allocation: the register file and the per-line
 * state arrays are embedded, so attaching one costs the caller's storage and
 * the bus callbacks stay allocation-free by construction. The observable
 * behaviour is copied from qemu-system-aarch64's `arm_gic` model (DT
 * compatible "arm,cortex-a15-gic", the DTB's /interrupt-controller node): two
 * memory-mapped regions -- a distributor (0x10000 bytes) and one per-CPU
 * interface (0x10000 bytes) -- the GICD_PIDR2 == 0x2B identity the driver
 * probes, a fixed 64-line (two-group) configuration, and a priority-scan CPU
 * interface that acknowledges the highest-precedence pending line and clears
 * it on EOI.
 *
 * This is a single-CPU, secure-only model: the interrupt lines live in one
 * bank, group (secure/non-secure) routing is accepted and echoed but does not
 * gate delivery (the guest runs at EL1 with no EL2 or GIC security extension
 * to split into), and the priority scan honours the full-priority byte and the
 * running-priority floor. A line with a lower priority *value* is higher
 * precedence, matching the architecture.
 *
 * External sources (the PL011, the arch timer, an SGI) reach the core through
 * two seams only: oemu_gicv2_set_pending drives a line's pending state and
 * oemu_gicv2_set_irq raises the single GICn_IRQ output the vCPU samples at its
 * quantum boundary. Nothing is wired until a machine assembles these -- see
 * `oemu boot` in src/main.c.
 */
#ifndef OEMU_GICV2_H
#define OEMU_GICV2_H

#include "oemu/device.h"
#include "oemu/macros.h"
#include "oemu/status.h"

#include <stdbool.h>
#include <stdint.h>

OEMU_BEGIN_DECLS

/* Lines the model tracks. Two groups of 32 (SGI 0-15, PPI 16-31, SPI 32-63):
 * GICD_TYPER reports itlines == 1 (== two groups) so the guest sizes its
 * arrays to exactly this, and NR_IRQS comes out 64 as the oracle prints. */
#define OEMU_GICV2_LINES 64U

/* Sentinel the CPU interface returns from the IAR when nothing is to be
 * acknowledged -- the value Linux's gic_handle_irq treats as "spurious". */
#define OEMU_GICV2_SPURIOUS 1023U

/*
 * The GIC exposes TWO memory-mapped regions, so it carries two device-op
 * tables. Attach the distributor with dist_ops and the CPU interface with
 * cpu_ops at their own bases; both share the one `ctx` (this struct).
 */
typedef struct oemu_gicv2 {
  oemu_device_ops dist_ops; /* attach at the distributor base */
  oemu_device_ops cpu_ops;  /* attach at the CPU interface base */
  /* Distributor control + identification. */
  uint32_t ctl;   /* GICD_CTL: bit0 enable */
  uint32_t typer; /* GICD_TYPER: itlines in bits[4:0] */
  uint32_t iidr;  /* GICD_IIDR: implementation id (0 = generic) */
  /* CPU interface (per-CPU; a single core is modelled). */
  uint32_t cpu_ctl;     /* GIC_CPU_CTL: bit0 enable */
  uint32_t cpu_pmr;     /* GIC_CPU_PMR: priority mask (drop lines >= this) */
  uint32_t cpu_bpr;     /* GIC_CPU_BPR: binary point (group/sub split) */
  uint32_t running_pri; /* current running priority: 256 when none active */
  uint32_t ccpner;      /* highest active priority (for the sysreg path) */
  /* Per-line state. SGI/PPI/SPI share one bank; indexed by interrupt id. */
  uint8_t enable[OEMU_GICV2_LINES];
  uint8_t pending[OEMU_GICV2_LINES]; /* edge latch / level source level */
  uint8_t active[OEMU_GICV2_LINES];
  uint8_t config[OEMU_GICV2_LINES]; /* bits[1:0] per line (0=edge,1=level) */
  uint8_t group[OEMU_GICV2_LINES];
  uint8_t priority[OEMU_GICV2_LINES]; /* lower value == higher precedence */
  uint8_t target[OEMU_GICV2_LINES];   /* CPU target mask (1 = this CPU) */
  /* Sticky machine event the driver may raise (unused in v2 but kept for
   * symmetry); none of the v2 registers map onto it today. */
  uint64_t sgir; /* GICD_SGIR last write, for tests */
} oemu_gicv2;

/*
 * Reset state: GIC disabled, every line masked/disabled with priority 0x80,
 * itlines set so the guest sees `lines` interrupts (must be a multiple of 32
 * and at most OEMU_GICV2_LINES). No allocation; safe on the stack.
 */
void oemu_gicv2_init(oemu_gicv2 *gic, unsigned lines);

/*
 * Drive one line's pending state, as a wired source would. `level` true
 * raises the line, false clears it (the model latches the value; edge
 * semantics are the source's concern, the GIC just sees pending set/clear).
 * Ignored for an out-of-range id.
 */
void oemu_gicv2_set_pending(oemu_gicv2 *gic, unsigned intid, bool level);

/*
 * The single interrupt output for the core: nonzero exactly when some
 * line is pending, enabled, targeted here, and beats the running priority
 * and the priority mask -- so the vCPU takes an IRQ. Wire this to
 * oemu_vcpu_set_irq at the run loop's quantum boundary.
 */
int oemu_gicv2_irq_level(const oemu_gicv2 *gic);

/* Number of lines this controller reports (a multiple of 32). */
unsigned oemu_gicv2_nr_lines(const oemu_gicv2 *gic);

OEMU_END_DECLS

#endif /* OEMU_GICV2_H */
