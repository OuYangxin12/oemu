/*
 * The generic-timer expiry predicate -- see include/oemu/gtimer.h.
 *
 * Deliberately a free function over (counter, ctl, cval): no device struct, no
 * state, nothing to initialise. The boot loop owns the counter (it advances
 * with the vCPU) and the distributor (it turns a level into a PPI); this file
 * only answers "is this comparator firing right now", which keeps the one
 * subtle rule -- enabled, unmasked, and passed -- in a place a unit test can
 * drive exhaustively without a running guest.
 */

#include "oemu/gtimer.h"

int oemu_gtimer_pending(uint64_t counter, uint64_t ctl, uint64_t cval) {
  if ((ctl & OEMU_GTIMER_CTL_ENABLE) == 0U) {
    return 0;
  }
  if ((ctl & OEMU_GTIMER_CTL_IMASK) != 0U) {
    return 0;
  }
  /* Unsigned wrap-correct compare: a counter that has wrapped past cval is
   * still expired, exactly as the subtract-with-sign hardware would say. */
  return ((int64_t)(counter - cval) >= 0) ? 1 : 0;
}
