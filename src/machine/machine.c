/*
 * machine.c -- the physical address space and the lifecycle events.
 *
 * Construction is the only place with real work: init raises the RAM region
 * out of the bus, so a machine has exactly two allocations -- the aspace
 * region table and the RAM block. Any failure half-way through disposes what
 * was built and leaves the struct zeroed, which makes both "dispose after a
 * failed init" and "re-init after a failed init" well-defined rather than a
 * caller trap.
 *
 * The events are recorded, never acted upon: acting on a reset or a
 * power-down means tearing down vCPU state, and the machine does not own
 * vCPUs. The run loop that will own them polls event_peek at safe points.
 */
#include "oemu/machine.h"

#include "oemu/aspace.h"
#include "oemu/macros.h"
#include "oemu/status.h"

#include <string.h>

oemu_status oemu_machine_init(oemu_machine *machine, uint64_t ram_base, uint64_t ram_size,
                              unsigned region_capacity) {
  if ((machine == NULL) || (ram_size == 0) || (region_capacity == 0)) {
    return OEMU_ERR_INVALID_ARG;
  }
  memset(machine, 0, sizeof(*machine));
  oemu_status st = oemu_aspace_init(&machine->aspace, region_capacity);
  if (st != OEMU_OK) {
    return st;
  }
  /* A kernel image arrives through the bus's write path, so RAM is mapped
   * RWX here rather than loaded through a privileged back door. */
  st = oemu_aspace_map_ram(&machine->aspace, ram_base, ram_size, OEMU_PERM_ALL, NULL);
  if (st != OEMU_OK) {
    oemu_aspace_dispose(&machine->aspace);
    memset(machine, 0, sizeof(*machine));
    return st;
  }
  return OEMU_OK;
}

void oemu_machine_dispose(oemu_machine *machine) {
  if (machine == NULL) {
    return;
  }
  oemu_aspace_dispose(&machine->aspace);
  machine->event = OEMU_MACHINE_EVENT_NONE;
  machine->exit_code = 0;
}

void oemu_machine_poweroff(oemu_machine *machine, int exit_code) {
  if (machine == NULL) {
    return;
  }
  machine->event = OEMU_MACHINE_EVENT_POWERDOWN;
  machine->exit_code = exit_code;
}

void oemu_machine_reset(oemu_machine *machine) {
  if (machine == NULL) {
    return;
  }
  machine->event = OEMU_MACHINE_EVENT_RESET;
}

oemu_machine_event oemu_machine_event_peek(const oemu_machine *machine) {
  if (machine == NULL) {
    return OEMU_MACHINE_EVENT_NONE;
  }
  return machine->event;
}
