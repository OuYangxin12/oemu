/*
 * Growable byte buffer.
 *
 * Serves as the worked example for this skeleton: it has allocation failure
 * paths, arithmetic overflow edges and precondition checks, so it exercises
 * every testing technique the harness provides (plain assertions, OOM injection
 * through the allocator seam, and death tests for contract violations).
 */
#ifndef OEMU_BUFFER_H
#define OEMU_BUFFER_H

#include "oemu/macros.h"
#include "oemu/status.h"

#include <stddef.h>

OEMU_BEGIN_DECLS

/*
 * Declared in the header (not opaque) so it can live on the stack. Treat the
 * fields as read-only; use the accessors below in new code.
 */
typedef struct oemu_buffer {
  unsigned char *data;
  size_t len;
  size_t cap;
} oemu_buffer;

/*
 * Initialises an empty buffer. `initial_cap` may be 0, in which case no
 * allocation happens until the first append.
 * Returns OEMU_ERR_INVALID_ARG if `buf` is NULL, OEMU_ERR_NO_MEMORY on
 * allocation failure.
 */
OEMU_NODISCARD oemu_status oemu_buffer_init(oemu_buffer *buf, size_t initial_cap);

/* Releases the storage and resets the buffer. Safe on a zeroed or NULL buffer. */
void oemu_buffer_dispose(oemu_buffer *buf);

/* Drops the contents but keeps the allocated capacity. */
void oemu_buffer_clear(oemu_buffer *buf);

/* Appends `size` bytes from `bytes`. A `size` of 0 is a no-op and succeeds. */
OEMU_NODISCARD oemu_status oemu_buffer_append(oemu_buffer *buf, const void *bytes, size_t size);

/* Appends a NUL-terminated string, excluding the terminator. */
OEMU_NODISCARD oemu_status oemu_buffer_append_str(oemu_buffer *buf, const char *str);

/* Appends printf-formatted text. */
OEMU_NODISCARD oemu_status oemu_buffer_appendf(oemu_buffer *buf, const char *fmt, ...)
    OEMU_PRINTF(2, 3);

/* Ensures room for at least `additional` more bytes without reallocating. */
OEMU_NODISCARD oemu_status oemu_buffer_reserve(oemu_buffer *buf, size_t additional);

/*
 * Returns the contents as a NUL-terminated string. The terminator is written
 * beyond `len` and is not counted by oemu_buffer_len(). Returns NULL only when
 * the terminator cannot be allocated.
 */
const char *oemu_buffer_cstr(oemu_buffer *buf);

/* Number of bytes stored. Returns 0 for a NULL buffer. */
size_t oemu_buffer_len(const oemu_buffer *buf);

/* Current capacity in bytes. Returns 0 for a NULL buffer. */
size_t oemu_buffer_capacity(const oemu_buffer *buf);

/* Read-only view of the stored bytes; may be NULL when the buffer is empty. */
const unsigned char *oemu_buffer_data(const oemu_buffer *buf);

OEMU_END_DECLS

#endif /* OEMU_BUFFER_H */
