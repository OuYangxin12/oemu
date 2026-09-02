/*
 * Pluggable allocator.
 *
 * All library allocation funnels through these three functions. Tests replace
 * the backing implementation to simulate out-of-memory conditions and to assert
 * that every allocation is released -- a "seam" that pure C code needs, because
 * gmock cannot intercept free functions such as malloc directly.
 */
#ifndef OEMU_ALLOCATOR_H
#define OEMU_ALLOCATOR_H

#include "oemu/macros.h"

#include <stddef.h>

OEMU_BEGIN_DECLS

typedef struct oemu_allocator {
  void *(*alloc)(size_t size, void *user_data);
  void *(*realloc)(void *ptr, size_t new_size, void *user_data);
  void (*free)(void *ptr, void *user_data);
  void *user_data;
} oemu_allocator;

/* The default allocator, backed by malloc/realloc/free. Never NULL. */
const oemu_allocator *oemu_allocator_default(void);

/*
 * Installs `allocator` as the process-wide allocator; passing NULL restores the
 * default. Returns the allocator that was previously installed so callers can
 * restore it. Not thread-safe: intended for start-up and for test fixtures.
 */
const oemu_allocator *oemu_allocator_set(const oemu_allocator *allocator);

/* Returns the currently installed allocator. Never NULL. */
const oemu_allocator *oemu_allocator_get(void);

OEMU_END_DECLS

#endif /* OEMU_ALLOCATOR_H */
