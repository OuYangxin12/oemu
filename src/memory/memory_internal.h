/*
 * Internal interface of the memory module -- NOT part of the public API.
 *
 * The region layout lives here, together with the handful of pure functions
 * that carry the module's real correctness risk: containment arithmetic that
 * must not wrap, overlap detection, and the little-endian byte assembly that
 * must not depend on the host's byte order. Each is exhaustively testable as a
 * pure function, which is why none of them is static in memory.c.
 *
 * Rules for this pattern (see regs_internal.h): never installed, never
 * included by another module's public header, oemu_<module>_internal_ prefix.
 */
#ifndef OEMU_SRC_MEMORY_INTERNAL_H
#define OEMU_SRC_MEMORY_INTERNAL_H

#include "oemu/macros.h"
#include "oemu/memory.h"
#include "oemu/status.h"

#include <stdbool.h>
#include <stdint.h>

OEMU_BEGIN_DECLS

/* One mapped range. `host` points to memory the region may or may not own.
 * The typedef is what lets both the C implementation and the C++ tests say
 * `oemu_memory_region` without the struct keyword. */
typedef struct oemu_memory_region oemu_memory_region;

struct oemu_memory_region {
  uint64_t gva;
  uint64_t size;
  uint32_t perms;
  uint8_t *host;
  bool owned; /* free `host` on dispose when true */
};

/*
 * Shared preconditions for both map functions: nonzero size, nonzero perms, no
 * wrap of gva+size. Returns OEMU_OK, OEMU_ERR_INVALID_ARG or OEMU_ERR_OVERFLOW.
 */
oemu_status oemu_memory_internal_check_map_args(uint64_t gva, uint64_t size, uint32_t perms);

/* True when the two half-open ranges [a, a+as) and [b, b+bs) intersect. */
bool oemu_memory_internal_ranges_overlap(uint64_t a, uint64_t as, uint64_t b, uint64_t bs);

/*
 * Returns the region that fully contains [gva, gva+size) and satisfies
 * (region->perms & perms) == perms, or NULL. Containment is computed without
 * ever forming gva+size, so a range ending at the top of the address space
 * still resolves. A zero size never matches.
 */
const oemu_memory_region *oemu_memory_internal_find(const oemu_memory *mem, uint64_t gva,
                                                    uint64_t size, uint32_t perms);

/*
 * Little-endian byte assembly, host-endianness-independent. disassemble joins
 * `nbytes` (1..8) bytes into a zero-extended value; assemble writes the low
 * `nbytes` bytes of `value` out.
 */
uint64_t oemu_memory_internal_disassemble(const uint8_t *src, unsigned nbytes);
void oemu_memory_internal_assemble(uint8_t *dst, unsigned nbytes, uint64_t value);

OEMU_END_DECLS

#endif /* OEMU_SRC_MEMORY_INTERNAL_H */
