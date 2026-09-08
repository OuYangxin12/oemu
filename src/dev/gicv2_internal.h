/*
 * White-box seam for the GICv2 priority scan (M4b). Kept out of the public
 * header: it is the pure decision table the black-box device behaviour is
 * built on, and the place to pin the tricky invariants (empty table, all
 * masked, running-priority floor, equal-priority tie-break) without a full
 * bus fixture.
 */
#ifndef OEMU_DEV_GICV2_INTERNAL_H
#define OEMU_DEV_GICV2_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "oemu/gicv2.h"

OEMU_BEGIN_DECLS

/*
 * The distributor's decision, as a pure function. Returns the interrupt id
 * the CPU interface would acknowledge this instant -- the enabled, pending,
 * non-active line targeted at this CPU whose priority *value* is lowest
 * (highest precedence), that also clears the priority mask (`pmr`: a line is
 * dropped when its priority value is >= pmr) and the running-priority floor
 * (`running_pri`: a line cannot preempt a higher-precedence active line) --
 * or OEMU_GICV2_SPURIOUS when none qualifies. A lower `running_pri` (256
 * means "nothing active") admits fewer lines.
 *
 * Ties on priority break toward the lower interrupt id, so the table is a
 * total order and the scan is deterministic.
 */
unsigned oemu_gicv2_internal_scan(const uint8_t enable[OEMU_GICV2_LINES],
                                  const uint8_t pending[OEMU_GICV2_LINES],
                                  const uint8_t active[OEMU_GICV2_LINES],
                                  const uint8_t priority[OEMU_GICV2_LINES],
                                  const uint8_t target[OEMU_GICV2_LINES], uint32_t running_pri,
                                  uint32_t pmr, unsigned lines);

OEMU_END_DECLS

#endif /* OEMU_DEV_GICV2_INTERNAL_H */
