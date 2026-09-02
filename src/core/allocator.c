#include "oemu/allocator.h"

#include <stdlib.h>

static void *default_alloc(size_t size, void *user_data) {
  (void)user_data;
  return malloc(size);
}

static void *default_realloc(void *ptr, size_t new_size, void *user_data) {
  (void)user_data;
  return realloc(ptr, new_size);
}

static void default_free(void *ptr, void *user_data) {
  (void)user_data;
  free(ptr);
}

static const oemu_allocator g_default_allocator = {
    default_alloc,
    default_realloc,
    default_free,
    NULL,
};

/* Indirection point the tests swap out; never NULL. */
static const oemu_allocator *g_current_allocator = &g_default_allocator;

const oemu_allocator *oemu_allocator_default(void) {
  return &g_default_allocator;
}

const oemu_allocator *oemu_allocator_set(const oemu_allocator *allocator) {
  const oemu_allocator *previous = g_current_allocator;
  g_current_allocator = (allocator != NULL) ? allocator : &g_default_allocator;
  return previous;
}

const oemu_allocator *oemu_allocator_get(void) {
  return g_current_allocator;
}
