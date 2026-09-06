/*
 * White-box views of the vCPU.
 *
 * The interrupt-delivery decision is pure state arithmetic -- pins, the DAIF
 * mask, priority -- and it is the part a guest can make deadlock by one
 * bit-slip, so the tests enumerate it directly instead of via step timing.
 */
#ifndef OEMU_SRC_VCPU_INTERNAL_H
#define OEMU_SRC_VCPU_INTERNAL_H

#include "oemu/exc.h"
#include "oemu/macros.h"

#include <stdbool.h>
#include <stdint.h>

OEMU_BEGIN_DECLS

/*
 * The kind of interrupt to deliver right now, or -1 for none: FIQ beats IRQ
 * (ARM priority order), and a masked pin is not pending at all -- a pending
 * but masked interrupt must not wake a WFI either, so this one function is
 * the whole wake question. `pstate` is the live PSTATE value.
 */
int oemu_vcpu_internal_pending_kind(uint64_t pstate, bool irq_level, bool fiq_level);

OEMU_END_DECLS

#endif /* OEMU_SRC_VCPU_INTERNAL_H */
