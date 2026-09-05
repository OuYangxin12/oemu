/*
 * The machine: a physical address space plus the lifecycle events a guest
 * can ask of it.
 *
 * A machine is deliberately small at this stage. It owns the address-space
 * table and one contiguous block of RAM, and it records rather than acts on
 * the two events a guest firmware interface can eventually trigger -- an
 * orderly power-down and a reset -- because acting on them belongs to the
 * layer that owns the vCPUs and the CLI loop (later phases). Recording them
 * here, at the point where the semantics live, is what lets every phase's
 * tests assert the contract long before any of that machinery exists.
 */
#ifndef OEMU_MACHINE_H
#define OEMU_MACHINE_H

#include "oemu/aspace.h"
#include "oemu/macros.h"
#include "oemu/status.h"

#include <stdint.h>

OEMU_BEGIN_DECLS

/*
 * Sticky machine events. A device callback (a PSCI SYSTEM_OFF in a later
 * phase) raises one via oemu_machine_poweroff / oemu_machine_reset; the
 * run loop polls it at safe points and stops. The event stays set until
 * dispose so a test can observe it after the run returns.
 */
typedef enum oemu_machine_event {
  OEMU_MACHINE_EVENT_NONE = 0,
  OEMU_MACHINE_EVENT_POWERDOWN = 1, /* the guest asked to be turned off */
  OEMU_MACHINE_EVENT_RESET = 2      /* the guest asked to start over */
} oemu_machine_event;

/*
 * Not opaque so it can live on the stack. Two allocations exist per live
 * machine: the aspace region table and the RAM block -- which is what makes
 * the OOM paths exhaustively testable, exactly as for oemu_memory.
 */
typedef struct oemu_machine {
  oemu_aspace aspace;       /* the machine's bus; compose devices onto it */
  oemu_machine_event event; /* sticky; OEMU_MACHINE_EVENT_NONE when running */
  int exit_code;            /* meaningful with POWERDOWN: the guest's wish */
} oemu_machine;

/*
 * Builds a machine with `ram_size` bytes of RAM at `ram_base` (RWX -- a
 * kernel image arrives through the bus's own write path, not through
 * special-cased permissions) and room for `region_capacity` regions total.
 * Returns OEMU_ERR_INVALID_ARG on a NULL machine, zero RAM size or zero
 * region capacity, OEMU_ERR_OVERFLOW if the RAM range wraps, OEMU_ERR_NO_MEMORY
 * if either allocation fails (on failure the machine is zeroed: dispose it or
 * re-init it, both are safe).
 */
OEMU_NODISCARD oemu_status oemu_machine_init(oemu_machine *machine, uint64_t ram_base,
                                             uint64_t ram_size, unsigned region_capacity);

/* Releases the RAM block and the region table. Safe to call twice, and on a
 * machine whose init failed; the struct is zeroed afterwards. */
void oemu_machine_dispose(oemu_machine *machine);

/* Raises the power-down event with the exit code the guest requested (the
 * PSCI SYSTEM_OFF parameter, once PSCI exists). */
void oemu_machine_poweroff(oemu_machine *machine, int exit_code);

/* Raises the reset event. Rebuilding guest state is the run loop's job; the
 * machine only records that it was asked. */
void oemu_machine_reset(oemu_machine *machine);

/* The current sticky event. A run loop stops when this is not NONE. */
oemu_machine_event oemu_machine_event_peek(const oemu_machine *machine);

OEMU_END_DECLS

#endif /* OEMU_MACHINE_H */
