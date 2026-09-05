/*
 * Internal interface of the aspace module -- NOT part of the public API.
 *
 * The region layout lives here, together with the pure functions that carry
 * this module's correctness risk: containment arithmetic that must not wrap,
 * overlap detection, and the little-endian byte assembly that must not depend
 * on the host's byte order. Each is exhaustively testable as a pure function,
 * which is why none of them is static in aspace.c.
 *
 * The overlap/contain/assemble helpers mirror src/memory/memory_internal.h on
 * purpose. The duplication is the cheaper option while the two tables stay
 * different (a memory region is RAM-only; an aspace region may be a device):
 * a shared "core" module both depend on would only pay for itself once a
 * third consumer appears.
 *
 * Rules for this pattern (see memory_internal.h): never installed, never
 * included by another module's public header, oemu_<module>_internal_ prefix.
 */
#ifndef OEMU_SRC_ASPACE_INTERNAL_H
#define OEMU_SRC_ASPACE_INTERNAL_H

#include "oemu/aspace.h"
#include "oemu/macros.h"
#include "oemu/status.h"

#include <stdbool.h>
#include <stdint.h>

OEMU_BEGIN_DECLS

typedef enum oemu_region_kind {
  OEMU_REGION_RAM = 0,   /* answered from `host` */
  OEMU_REGION_DEVICE = 1 /* answered through `ops` */
} oemu_region_kind;

/* One attached region. `host` backs RAM regions (owned or alias); `ops`
 * serves device regions. The typedef lets C++ tests name the type without
 * the struct keyword. */
typedef struct oemu_aspace_region oemu_aspace_region;

struct oemu_aspace_region {
  uint64_t pa;
  uint64_t size;
  uint32_t perms;
  oemu_region_kind kind;
  uint8_t *host;
  bool owned; /* free `host` on dispose when true */
  const oemu_device_ops *ops;
};

/*
 * Shared preconditions for every map/attach: nonzero size, nonzero perms, no
 * wrap of pa+size. Returns OEMU_OK, OEMU_ERR_INVALID_ARG or OEMU_ERR_OVERFLOW.
 */
oemu_status oemu_aspace_internal_check_map_args(uint64_t pa, uint64_t size, uint32_t perms);

/* True when the two half-open ranges [a, a+as) and [b, b+bs) intersect. */
bool oemu_aspace_internal_ranges_overlap(uint64_t a, uint64_t as, uint64_t b, uint64_t bs);

/*
 * Returns the region that fully contains [pa, pa+size) and satisfies
 * (region->perms & perms) == perms, or NULL. Containment is computed without
 * ever forming pa+size, so a region ending at the top of the address space
 * still resolves. A zero size never matches.
 */
const oemu_aspace_region *oemu_aspace_internal_find(const oemu_aspace *as, uint64_t pa,
                                                    uint64_t size, uint32_t perms);

/*
 * Little-endian byte assembly, host-endianness-independent. disassemble joins
 * `nbytes` (1..8) bytes into a zero-extended value; assemble writes the low
 * `nbytes` bytes of `value` out.
 */
uint64_t oemu_aspace_internal_disassemble(const uint8_t *src, unsigned nbytes);
void oemu_aspace_internal_assemble(uint8_t *dst, unsigned nbytes, uint64_t value);

OEMU_END_DECLS

#endif /* OEMU_SRC_ASPACE_INTERNAL_H */
