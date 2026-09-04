/*
 * Region-table guest memory. See include/oemu/memory.h for the contract.
 *
 * Two invariants hold the module together:
 *
 *   1. Containment arithmetic never forms gva+size. The address space is 64
 *      bits wide and a mapping is allowed to run to its very top, so every
 *      "is [g, g+s) inside [r, r+rs)?" test is written as two subtractions
 *      (g >= r && s <= rs && g-r <= rs-s). Forming either sum would wrap.
 *
 *   2. Guest-visible values are assembled byte by byte. A memcpy straight into
 *      a uint64_t would make guest state depend on the host's byte order and
 *      alignment, and UBSan would rightly object to the punned pointer.
 */
#include <string.h>

#include "memory_internal.h"

oemu_status oemu_memory_internal_check_map_args(uint64_t gva, uint64_t size, uint32_t perms) {
  if (size == 0U || perms == 0U) {
    return OEMU_ERR_INVALID_ARG;
  }
  /* Overflow is size-1 versus the headroom: a range ending exactly at 2^64
   * is legal, and the end-exclusive top of the address space must stay
   * mappable. Comparing `size` here instead would refuse that last page. */
  if (size - 1U > (UINT64_MAX - gva)) {
    return OEMU_ERR_OVERFLOW;
  }
  return OEMU_OK;
}

bool oemu_memory_internal_ranges_overlap(uint64_t a, uint64_t as, uint64_t b, uint64_t bs) {
  /* Same no-sum rule as containment: a <= b is safe, b - a < as is not. */
  if (a <= b) {
    return (b - a) < as;
  }
  return (a - b) < bs;
}

const oemu_memory_region *oemu_memory_internal_find(const oemu_memory *mem, uint64_t gva,
                                                    uint64_t size, uint32_t perms) {
  if (mem == NULL || size == 0U) {
    return NULL;
  }
  for (unsigned i = 0U; i < mem->region_count; i++) {
    const oemu_memory_region *r = &mem->regions[i];
    if ((r->perms & perms) != perms) {
      continue;
    }
    /* r->gva <= gva, expressed without a sum that could wrap. */
    if (r->gva > gva) {
      continue;
    }
    /* gva - r->gva + size <= r->size, split into two subtractions: the first
     * guard keeps `r->size - off` from wrapping, the second does containment.
     * (Writing this as `off > r->size - size` wraps whenever the request is
     * larger than the region and wrongly reports a match -- found by test.) */
    const uint64_t off = gva - r->gva;
    if ((off >= r->size) || (size > (r->size - off))) {
      continue;
    }
    return r;
  }
  return NULL;
}

uint64_t oemu_memory_internal_disassemble(const uint8_t *src, unsigned nbytes) {
  uint64_t value = 0U;
  for (unsigned i = 0U; i < nbytes; i++) {
    value |= (uint64_t)src[i] << (8U * i);
  }
  return value;
}

void oemu_memory_internal_assemble(uint8_t *dst, unsigned nbytes, uint64_t value) {
  for (unsigned i = 0U; i < nbytes; i++) {
    dst[i] = (uint8_t)((value >> (8U * i)) & UINT64_C(0xFF));
  }
}

oemu_status oemu_memory_init(oemu_memory *mem, unsigned region_capacity) {
  if (mem == NULL || region_capacity == 0U) {
    return OEMU_ERR_INVALID_ARG;
  }
  const oemu_allocator *alloc = oemu_allocator_get();
  void *table =
      alloc->alloc(sizeof(oemu_memory_region) * (size_t)region_capacity, alloc->user_data);
  if (table == NULL) {
    return OEMU_ERR_NO_MEMORY;
  }
  mem->regions = (oemu_memory_region *)table;
  mem->allocator = alloc;
  mem->region_count = 0U;
  mem->region_capacity = region_capacity;
  return OEMU_OK;
}

void oemu_memory_dispose(oemu_memory *mem) {
  if (mem == NULL) {
    return;
  }
  if (mem->regions != NULL) {
    for (unsigned i = 0U; i < mem->region_count; i++) {
      if (mem->regions[i].owned) {
        mem->allocator->free(mem->regions[i].host, mem->allocator->user_data);
      }
    }
    mem->allocator->free(mem->regions, mem->allocator->user_data);
  }
  mem->regions = NULL;
  mem->allocator = NULL;
  mem->region_count = 0U;
  mem->region_capacity = 0U;
}

static oemu_status memory_map(oemu_memory *mem, uint64_t gva, void *host, uint64_t size,
                              uint32_t perms, bool owned, const void *contents,
                              uint64_t contents_size) {
  if (mem == NULL || mem->regions == NULL) {
    return OEMU_ERR_INVALID_ARG;
  }
  const oemu_status args = oemu_memory_internal_check_map_args(gva, size, perms);
  if (args != OEMU_OK) {
    return args;
  }
  if (mem->region_count >= mem->region_capacity) {
    return OEMU_ERR_RANGE;
  }
  for (unsigned i = 0U; i < mem->region_count; i++) {
    const oemu_memory_region *r = &mem->regions[i];
    if (oemu_memory_internal_ranges_overlap(gva, size, r->gva, r->size)) {
      return OEMU_ERR_RANGE;
    }
  }

  if (owned) {
    if (contents_size > size || (contents == NULL && contents_size > 0U)) {
      return OEMU_ERR_INVALID_ARG;
    }
    void *block = mem->allocator->alloc((size_t)size, mem->allocator->user_data);
    if (block == NULL) {
      return OEMU_ERR_NO_MEMORY;
    }
    /* Backing memory starts zeroed so a mapped-but-unwritten guest region
     * reads back deterministically regardless of what malloc handed us; a file
     * slice then overwrites its prefix, leaving any .bss tail zero. */
    (void)memset(block, 0, (size_t)size);
    if (contents_size > 0U) {
      (void)memcpy(block, contents, (size_t)contents_size);
    }
    host = block;
  }

  oemu_memory_region *r = &mem->regions[mem->region_count];
  r->gva = gva;
  r->size = size;
  r->perms = perms;
  r->host = (uint8_t *)host;
  r->owned = owned;
  mem->region_count++;
  return OEMU_OK;
}

oemu_status oemu_memory_map(oemu_memory *mem, uint64_t gva, uint64_t size, uint32_t perms) {
  return memory_map(mem, gva, NULL, size, perms, true, NULL, 0U);
}

oemu_status oemu_memory_map_image(oemu_memory *mem, uint64_t gva, uint64_t size, uint32_t perms,
                                  const void *contents, uint64_t contents_size) {
  return memory_map(mem, gva, NULL, size, perms, true, contents, contents_size);
}

oemu_status oemu_memory_map_alias(oemu_memory *mem, uint64_t gva, void *host, uint64_t size,
                                  uint32_t perms) {
  if (host == NULL) {
    return OEMU_ERR_INVALID_ARG;
  }
  return memory_map(mem, gva, host, size, perms, false, NULL, 0U);
}

oemu_status oemu_memory_validate(const oemu_memory *mem, uint64_t gva, uint64_t size,
                                 uint32_t perms) {
  if (mem == NULL || size == 0U) {
    return OEMU_ERR_INVALID_ARG;
  }
  return (oemu_memory_internal_find(mem, gva, size, perms) != NULL) ? OEMU_OK : OEMU_ERR_FAULT;
}

oemu_status oemu_memory_read(const oemu_memory *mem, uint64_t gva, oemu_mem_size size,
                             bool sign_extend, uint64_t *value_out) {
  if (value_out == NULL) {
    return OEMU_ERR_INVALID_ARG;
  }
  const unsigned nbytes = 1U << (unsigned)size;
  const oemu_memory_region *r = oemu_memory_internal_find(mem, gva, nbytes, OEMU_PERM_READ);
  if (r == NULL) {
    return OEMU_ERR_FAULT;
  }
  uint8_t bytes[8];
  (void)memcpy(bytes, r->host + (gva - r->gva), nbytes);
  uint64_t value = oemu_memory_internal_disassemble(bytes, nbytes);
  if (sign_extend && (value & (UINT64_C(1) << (8U * nbytes - 1U))) != 0U) {
    value |= ~(UINT64_MAX >> (64U - 8U * nbytes));
  }
  *value_out = value;
  return OEMU_OK;
}

oemu_status oemu_memory_write(const oemu_memory *mem, uint64_t gva, oemu_mem_size size,
                              uint64_t value) {
  const unsigned nbytes = 1U << (unsigned)size;
  const oemu_memory_region *r = oemu_memory_internal_find(mem, gva, nbytes, OEMU_PERM_WRITE);
  if (r == NULL) {
    return OEMU_ERR_FAULT;
  }
  uint8_t bytes[8];
  oemu_memory_internal_assemble(bytes, nbytes, value);
  (void)memcpy(r->host + (gva - r->gva), bytes, nbytes);
  return OEMU_OK;
}

oemu_status oemu_memory_write_bytes(const oemu_memory *mem, uint64_t gva, const void *src,
                                    uint64_t size) {
  if (size == 0U) {
    return OEMU_OK; /* a no-op copy, including from a NULL source */
  }
  if (src == NULL) {
    return OEMU_ERR_INVALID_ARG;
  }
  const oemu_memory_region *r = oemu_memory_internal_find(mem, gva, size, OEMU_PERM_WRITE);
  if (r == NULL) {
    return OEMU_ERR_FAULT;
  }
  (void)memcpy(r->host + (gva - r->gva), src, (size_t)size);
  return OEMU_OK;
}

oemu_status oemu_memory_fetch32(const oemu_memory *mem, uint64_t pc, uint32_t *word_out) {
  if (word_out == NULL) {
    return OEMU_ERR_INVALID_ARG;
  }
  /* AArch64 fetches are always word-aligned; a misaligned PC is a fault the
   * architecture itself raises, so it is reported, not silently rounded. */
  if ((pc & (UINT64_C(4) - UINT64_C(1))) != 0U) {
    return OEMU_ERR_FAULT;
  }
  const oemu_memory_region *r = oemu_memory_internal_find(mem, pc, UINT64_C(4), OEMU_PERM_EXEC);
  if (r == NULL) {
    return OEMU_ERR_FAULT;
  }
  uint8_t bytes[4];
  (void)memcpy(bytes, r->host + (pc - r->gva), sizeof(bytes));
  *word_out = (uint32_t)oemu_memory_internal_disassemble(bytes, sizeof(bytes));
  return OEMU_OK;
}
