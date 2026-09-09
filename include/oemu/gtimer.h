/*
 * The ARMv8 generic timer -- just enough of it to advance the guest's jiffies.
 *
 * The kernel's arm_arch_timer clockevent programs a comparator (CNTP_CVAL /
 * CNTV_CVAL) against the counter; when the counter passes it and the timer is
 * enabled, the CPU interface takes a PPI. oemu advances the counter with guest
 * progress (vcpu.c), so the whole device is this one predicate -- and the
 * counter is a parameter rather than a wall clock, which is what makes the
 * "did the tick fire" behaviour testable without a timing race.
 *
 * This is not a time-of-day model: there is no alarm, no offset drift, no
 * second timer. It exists so the kernel sees a moving jiffies and a live
 * clockevent, which is the difference between an idle guest that makes
 * forward progress and one that soft-locks waiting for a tick that never comes.
 */
#ifndef OEMU_GTIMER_H
#define OEMU_GTIMER_H

#include "oemu/macros.h"

#include <stdint.h>

OEMU_BEGIN_DECLS

/* CNTP_CTL / CNTV_CTL bit positions (ARMv8.4, same layout for both). */
#define OEMU_GTIMER_CTL_ENABLE 0x1U
#define OEMU_GTIMER_CTL_IMASK  0x2U
/* Read-only: set while the timer condition holds and the interrupt is not
 * masked. Linux's arch timer handler gates its re-arm on this bit. */
#define OEMU_GTIMER_CTL_ISTAT  0x4U

/*
 * Does a timer output assert, given the counter it compares against, its
 * control word and its comparator value? Level-high while enabled, unmasked
 * and expired; the kernel's handler reprograms CVAL forward, which drops the
 * level just as a real timer re-arms. Passing `counter` in (rather than
 * reading a clock) is the testability seam: a unit test pins a counter, a
 * control word and a comparator and knows the exact answer.
 */
int oemu_gtimer_pending(uint64_t counter, uint64_t ctl, uint64_t cval);

OEMU_END_DECLS

#endif /* OEMU_GTIMER_H */
