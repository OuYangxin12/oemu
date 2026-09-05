/*
 * Guest physical address space: RAM regions plus MMIO device regions.
 *
 * Where oemu_memory is the flat VA table the user-mode facade uses (VA == PA,
 * everything RAM), this is the bus a system-mode machine hangs devices off:
 * the same region-table lookup, but a hit may land on RAM (a byte-assembled
 * host access) or on a device (a call into oemu_device_ops). Physical regions
 * are registered once and never unmapped -- page-table changes alter
 * translation, not this map -- which keeps the module's allocation story as
 * simple as the flat one: the table, plus one owned block per mapped RAM
 * region, and nothing allocated by a device attach or by an access.
 *
 * All guest-visible access is little-endian and byte-assembled, so the host's
 * own byte order never leaks into guest state.
 */
#ifndef OEMU_ASPACE_H
#define OEMU_ASPACE_H

#include "oemu/allocator.h"
#include "oemu/decode.h" /* oemu_mem_size: the transfer-size selector */
#include "oemu/device.h"
#include "oemu/macros.h"
#include "oemu/memops.h" /* the bus view returned by oemu_aspace_memops */
#include "oemu/memory.h" /* the OEMU_PERM_* bits are shared vocabulary */
#include "oemu/status.h"

#include <stdbool.h>
#include <stdint.h>

OEMU_BEGIN_DECLS

struct oemu_aspace_region; /* defined in src/aspace/aspace_internal.h */

/*
 * The struct is not opaque so it can live on the stack; treat the fields as
 * read-only and go through the functions. Exactly one allocation exists per
 * oemu_aspace_init, plus one backing block per oemu_aspace_map_ram region;
 * oemu_aspace_attach_device allocates nothing, so a machine's device
 * topology carries no hidden lifetime.
 */
typedef struct oemu_aspace {
  struct oemu_aspace_region *regions;
  const oemu_allocator *allocator;
  unsigned region_count;
  unsigned region_capacity;
} oemu_aspace;

/*
 * Empties the bus with room for `region_capacity` regions. Allocates the
 * region table from the installed allocator. Returns OEMU_ERR_INVALID_ARG on
 * NULL or a zero capacity, OEMU_ERR_NO_MEMORY if the table cannot be
 * allocated (the struct is left zeroed and unusable).
 */
OEMU_NODISCARD oemu_status oemu_aspace_init(oemu_aspace *as, unsigned region_capacity);

/* Frees the region table and every owned RAM block. Device regions are not
 * owned: their ctx belongs to the device model that built it. Safe to call
 * twice; the struct is zeroed afterwards. */
void oemu_aspace_dispose(oemu_aspace *as);

/*
 * Maps `size` freshly allocated, zero-filled RAM at [`pa`, `pa+size`) with
 * `perms`. When `host_out` is non-NULL it receives the backing pointer, so a
 * loader or a test can poke bytes without pretending to be the guest.
 *
 * OEMU_ERR_INVALID_ARG: NULL bus, zero size, or zero perms.
 * OEMU_ERR_OVERFLOW: the range wraps past 2^64.
 * OEMU_ERR_RANGE: overlaps an existing region, or the table is full.
 * OEMU_ERR_NO_MEMORY: the backing block cannot be allocated (the bus stays
 * exactly as it was -- a failed map changes nothing observable).
 */
OEMU_NODISCARD oemu_status oemu_aspace_map_ram(oemu_aspace *as, uint64_t pa, uint64_t size,
                                               uint32_t perms, void **host_out);

/*
 * Maps a caller-owned host buffer as RAM at `pa`. Nothing is allocated and
 * nothing is copied; the buffer must outlive the mapping. Same errors as
 * oemu_aspace_map_ram, plus OEMU_ERR_INVALID_ARG for a NULL host.
 */
OEMU_NODISCARD oemu_status oemu_aspace_map_ram_alias(oemu_aspace *as, uint64_t pa, void *host,
                                                     uint64_t size, uint32_t perms);

/*
 * Attaches `ops` over [`pa`, `pa+size`) as an MMIO region. Device regions
 * carry READ|WRITE permissions implicitly and are never executable. Allocates
 * nothing. Both ops->read and ops->write must be non-NULL. Same region-table
 * errors as the map functions.
 */
OEMU_NODISCARD oemu_status oemu_aspace_attach_device(oemu_aspace *as, uint64_t pa,
                                                     uint64_t size, const oemu_device_ops *ops);

/*
 * Reports whether [pa, pa+size) lies wholly inside one region carrying at
 * least `perms`, without performing the access. OEMU_ERR_FAULT on unmapped,
 * cross-region or under-permissioned; OEMU_ERR_INVALID_ARG for a NULL bus or
 * a zero size. A device region satisfies any READ/WRITE query as a whole:
 * register-level validity is the device's decision, reported as OEMU_ERR_FAULT
 * from its callbacks, because only the device knows which offsets exist.
 */
OEMU_NODISCARD oemu_status oemu_aspace_validate(const oemu_aspace *as, uint64_t pa,
                                                uint64_t size, uint32_t perms);

/*
 * Reads `size` little-endian bytes from `pa`, optionally sign-extending the
 * loaded value to 64 bits. RAM regions answer from their backing block;
 * device regions call ops->read with the device-relative offset. A device
 * access must be naturally aligned -- the bus reports OEMU_ERR_FAULT, the
 * architectural Alignment fault, rather than let the device see a misaligned
 * register access. OEMU_ERR_FAULT whenever oemu_aspace_validate would fail.
 */
OEMU_NODISCARD oemu_status oemu_aspace_read(const oemu_aspace *as, uint64_t pa,
                                            oemu_mem_size size, bool sign_extend,
                                            uint64_t *value_out);

/*
 * Writes the low (1 << size) little-endian bytes of `value`. Same routing,
 * same alignment rule for devices as oemu_aspace_read; a device callback sees
 * only the bytes that were transferred.
 */
OEMU_NODISCARD oemu_status oemu_aspace_write(const oemu_aspace *as, uint64_t pa,
                                             oemu_mem_size size, uint64_t value);

/*
 * Fetches the 4-byte instruction word at `pa`. Requires OEMU_PERM_EXEC and a
 * 4-byte-aligned address. A device region is never executable, so fetching
 * from one reports OEMU_ERR_FAULT exactly like a misaligned or under-
 * permissioned fetch.
 */
OEMU_NODISCARD oemu_status oemu_aspace_fetch32(const oemu_aspace *as, uint64_t pa,
                                               uint32_t *word_out);

/*
 * A bus view of this address space: an oemu_memops whose callbacks call
 * straight back into the entry points above, for handing to
 * oemu_exec_step_bus(). The view borrows: `as` must outlive it, and later
 * map/attach calls are visible through it (the callbacks re-read the table).
 * Constructing it allocates nothing.
 */
OEMU_NODISCARD oemu_memops oemu_aspace_memops(oemu_aspace *as);

OEMU_END_DECLS

#endif /* OEMU_ASPACE_H */
