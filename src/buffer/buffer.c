#include "oemu/buffer.h"

#include <stdarg.h>
#include <stdint.h> /* SIZE_MAX */
#include <stdio.h>
#include <string.h>

#include "buffer_internal.h"
#include "oemu/allocator.h"

/* --- internal helpers (exposed via buffer_internal.h for white-box tests) --- */

oemu_status oemu_buffer_internal_checked_add(size_t a, size_t b, size_t *out) {
  if (out == NULL) {
    return OEMU_ERR_INVALID_ARG;
  }
  if (a > SIZE_MAX - b) {
    return OEMU_ERR_OVERFLOW;
  }
  *out = a + b;
  return OEMU_OK;
}

oemu_status oemu_buffer_internal_grow_capacity(size_t current_cap, size_t required,
                                              size_t *out_cap) {
  if (out_cap == NULL) {
    return OEMU_ERR_INVALID_ARG;
  }

  if (current_cap >= required) {
    *out_cap = current_cap;
    return OEMU_OK;
  }

  size_t cap = (current_cap < OEMU_BUFFER_MIN_CAP) ? OEMU_BUFFER_MIN_CAP : current_cap;

  /* Geometric 1.5x growth, guarding every step against wrap-around. */
  while (cap < required) {
    size_t half = cap / 2u;
    if (cap > SIZE_MAX - half) {
      /* Cannot grow geometrically any further: fall back to the exact size. */
      cap = required;
      break;
    }
    size_t next = cap + half;
    if (next <= cap) { /* defensive: no forward progress */
      cap = required;
      break;
    }
    cap = next;
  }

  *out_cap = cap;
  return OEMU_OK;
}

/* Grows the buffer so that at least `additional` more bytes fit. */
static oemu_status buffer_grow(oemu_buffer *buf, size_t additional) {
  size_t required = 0;
  oemu_status status = oemu_buffer_internal_checked_add(buf->len, additional, &required);
  if (status != OEMU_OK) {
    return status;
  }

  if (required <= buf->cap) {
    return OEMU_OK;
  }

  size_t new_cap = 0;
  status = oemu_buffer_internal_grow_capacity(buf->cap, required, &new_cap);
  if (status != OEMU_OK) {
    return status;
  }

  const oemu_allocator *alloc = oemu_allocator_get();
  unsigned char *data = (unsigned char *)alloc->realloc(buf->data, new_cap, alloc->user_data);
  if (data == NULL) {
    /* buf->data is left untouched, so the buffer stays valid after failure. */
    return OEMU_ERR_NO_MEMORY;
  }

  buf->data = data;
  buf->cap = new_cap;
  return OEMU_OK;
}

/* --- public API --- */

oemu_status oemu_buffer_init(oemu_buffer *buf, size_t initial_cap) {
  if (buf == NULL) {
    return OEMU_ERR_INVALID_ARG;
  }

  buf->data = NULL;
  buf->len = 0;
  buf->cap = 0;

  if (initial_cap == 0) {
    return OEMU_OK;
  }

  const oemu_allocator *alloc = oemu_allocator_get();
  unsigned char *data = (unsigned char *)alloc->alloc(initial_cap, alloc->user_data);
  if (data == NULL) {
    return OEMU_ERR_NO_MEMORY;
  }

  buf->data = data;
  buf->cap = initial_cap;
  return OEMU_OK;
}

void oemu_buffer_dispose(oemu_buffer *buf) {
  if (buf == NULL) {
    return;
  }
  if (buf->data != NULL) {
    const oemu_allocator *alloc = oemu_allocator_get();
    alloc->free(buf->data, alloc->user_data);
  }
  buf->data = NULL;
  buf->len = 0;
  buf->cap = 0;
}

void oemu_buffer_clear(oemu_buffer *buf) {
  if (buf == NULL) {
    return;
  }
  buf->len = 0;
}

oemu_status oemu_buffer_reserve(oemu_buffer *buf, size_t additional) {
  if (buf == NULL) {
    return OEMU_ERR_INVALID_ARG;
  }
  return buffer_grow(buf, additional);
}

oemu_status oemu_buffer_append(oemu_buffer *buf, const void *bytes, size_t size) {
  if (buf == NULL) {
    return OEMU_ERR_INVALID_ARG;
  }
  if (size == 0) {
    return OEMU_OK;
  }
  if (bytes == NULL) {
    return OEMU_ERR_INVALID_ARG;
  }

  oemu_status status = buffer_grow(buf, size);
  if (status != OEMU_OK) {
    return status;
  }

  memcpy(buf->data + buf->len, bytes, size);
  buf->len += size;
  return OEMU_OK;
}

oemu_status oemu_buffer_append_str(oemu_buffer *buf, const char *str) {
  if (buf == NULL || str == NULL) {
    return OEMU_ERR_INVALID_ARG;
  }
  return oemu_buffer_append(buf, str, strlen(str));
}

oemu_status oemu_buffer_appendf(oemu_buffer *buf, const char *fmt, ...) {
  if (buf == NULL || fmt == NULL) {
    return OEMU_ERR_INVALID_ARG;
  }

  /* First pass: ask vsnprintf how many bytes the result needs. */
  va_list args;
  va_start(args, fmt);
  int needed = vsnprintf(NULL, 0, fmt, args);
  va_end(args);

  if (needed < 0) {
    return OEMU_ERR_INVALID_ARG;
  }
  if (needed == 0) {
    return OEMU_OK;
  }

  size_t needed_sz = (size_t)needed;

  /* +1 for the NUL that vsnprintf always writes; it is not part of len. */
  size_t with_nul = 0;
  oemu_status status = oemu_buffer_internal_checked_add(needed_sz, 1u, &with_nul);
  if (status != OEMU_OK) {
    return status;
  }

  status = buffer_grow(buf, with_nul);
  if (status != OEMU_OK) {
    return status;
  }

  va_start(args, fmt);
  int written = vsnprintf((char *)(buf->data + buf->len), with_nul, fmt, args);
  va_end(args);

  if (written < 0 || (size_t)written != needed_sz) {
    return OEMU_ERR_INVALID_ARG;
  }

  buf->len += needed_sz;
  return OEMU_OK;
}

const char *oemu_buffer_cstr(oemu_buffer *buf) {
  if (buf == NULL) {
    return NULL;
  }

  /* Reserve one byte for the terminator without counting it in len. */
  if (buffer_grow(buf, 1u) != OEMU_OK) {
    return NULL;
  }

  buf->data[buf->len] = '\0';
  return (const char *)buf->data;
}

size_t oemu_buffer_len(const oemu_buffer *buf) { return (buf != NULL) ? buf->len : 0u; }

size_t oemu_buffer_capacity(const oemu_buffer *buf) { return (buf != NULL) ? buf->cap : 0u; }

const unsigned char *oemu_buffer_data(const oemu_buffer *buf) {
  return (buf != NULL) ? buf->data : NULL;
}
