/*
 * Internal interface of the buffer module -- NOT part of the public API.
 *
 * This header exists so unit tests can reach the module's internal decision
 * logic without those functions being `static` and therefore untestable, and
 * without resorting to `#include "buffer.c"` from the test file.
 *
 * Rules for this pattern:
 *   - never installed, never included by another module's public header;
 *   - symbols carry the oemu_buffer_internal_ prefix to signal their status;
 *   - a change here is not a public API break.
 */
#ifndef OEMU_SRC_BUFFER_INTERNAL_H
#define OEMU_SRC_BUFFER_INTERNAL_H

#include <stddef.h>

#include "oemu/buffer.h"
#include "oemu/macros.h"
#include "oemu/status.h"

OEMU_BEGIN_DECLS

/* Smallest capacity allocated on the first growth. */
#define OEMU_BUFFER_MIN_CAP ((size_t)16)

/*
 * Growth policy: computes the new capacity needed to hold `required` bytes,
 * starting from `current_cap`. Grows geometrically (1.5x) and rounds up to at
 * least OEMU_BUFFER_MIN_CAP.
 *
 * Returns OEMU_ERR_OVERFLOW if no representable capacity satisfies `required`,
 * OEMU_ERR_INVALID_ARG if `out_cap` is NULL. Pure function: easy to test
 * exhaustively without allocating anything.
 */
OEMU_NODISCARD oemu_status oemu_buffer_internal_grow_capacity(size_t current_cap,
                                                              size_t required,
                                                              size_t *out_cap);

/*
 * Checked addition used by the append paths. Returns OEMU_ERR_OVERFLOW when
 * a + b would wrap.
 */
OEMU_NODISCARD oemu_status oemu_buffer_internal_checked_add(size_t a, size_t b, size_t *out);

OEMU_END_DECLS

#endif /* OEMU_SRC_BUFFER_INTERNAL_H */
