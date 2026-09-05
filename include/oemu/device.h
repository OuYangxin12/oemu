/*
 * MMIO device interface for the physical address space.
 *
 * A device is a range of guest physical addresses that is not backed by RAM:
 * reads and writes are intercepted and dispatched to callbacks. This header
 * defines only the calling contract; concrete device models (UART, GIC,
 * timer, ...) live in src/dev/ and are attached with oemu_aspace_attach_device.
 *
 * Contract every implementation must honour, because the alternative (a
 * device that allocates or blocks during an access) breaks the two invariants
 * the address space exists to keep:
 *
 *   - Never allocate. Any buffering (a UART TX ring, for instance) is set up
 *     at device-init time through the allocator seam, never during an access,
 *     so an instruction that touches a device stays allocation-free.
 *   - Never block the vCPU. Device callbacks run inline on the vCPU thread;
 *     waiting is expressed as state (a full queue), not as a host sleep.
 *
 * Offsets given to the callbacks are device-relative: the bus subtracts the
 * region base before calling, so a device model never learns its own address.
 */
#ifndef OEMU_DEVICE_H
#define OEMU_DEVICE_H

#include "oemu/decode.h" /* oemu_mem_size: the transfer-size selector */
#include "oemu/macros.h"
#include "oemu/status.h"

#include <stdint.h>

OEMU_BEGIN_DECLS

/*
 * The two bus operations a device must provide. `read` stores the
 * little-endian value of `1 << size` bytes at device-relative `offset` into
 * *value_out (zero-extended to 64 bits); `write` commits the low
 * `1 << size` bytes of `value`. A side effect (a FIFO pop, a bit that clears
 * on read) must not happen when the access fails, so validate before mutating.
 *
 * The returned status reaches the guest-visible access verbatim, so report
 * OEMU_ERR_FAULT for an offset the device does not implement -- that is the
 * architectural Data Abort, and the exception layer turns it into one.
 */
typedef struct oemu_device_ops {
  void *ctx; /* passed back to every callback; owned by the device */
  oemu_status (*read)(void *ctx, uint64_t offset, oemu_mem_size size, uint64_t *value_out);
  oemu_status (*write)(void *ctx, uint64_t offset, oemu_mem_size size, uint64_t value);
} oemu_device_ops;

OEMU_END_DECLS

#endif /* OEMU_DEVICE_H */
