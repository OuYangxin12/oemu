/*
 * aspace.c -- address-space dispatch: region table, RAM and device routing.
 *
 * A flat table of regions, first match wins, exactly as the RAM-only table in
 * src/memory/ does -- this space is deliberately small (tens of regions), and
 * a table keeps lookup allocation-free and its rules directly testable. The
 * difference is that a region is either RAM (answered from `host`) or a
 * device (answered through `ops`).
 *
 * Every bus access is checked for full containment in one region, so a load
 * may never straddle a region boundary -- no split fetches, no partial
 * device side-effects. Device accesses must additionally be naturally
 * aligned: the bus rejects a misaligned one with a fault before the model's
 * callback runs, because a device register file cannot honour a split
 * access. Device reads are normalised through the same mask/sign-extension
 * as RAM reads, so a model leaking garbage in the high bits can never be
 * observed by guest code.
 *
 * Allocation policy: the region table is allocated by init; map_ram allocates
 * the RAM image and owns it, map_ram_alias borrows it. A failed map leaves
 * the table untouched and leaks nothing. All allocation goes through the
 * allocator seam.
 */
#include "oemu/allocator.h"
#include "oemu/macros.h"
#include "oemu/status.h"

#include <string.h>

#include "aspace_internal.h"

/* The bus services accesses of one to eight bytes, selected by oemu_mem_size. */
static unsigned size_to_bytes(oemu_mem_size size) {
  switch (size) {
    case OEMU_MEM_BYTE:
      return 1;
    case OEMU_MEM_HALF:
      return 2;
    case OEMU_MEM_WORD:
      return 4;
    case OEMU_MEM_DWORD:
      return 8;
    default:
      return 0;
  }
}

/*
 * Bus-side normalisation of a loaded value: mask to the access width, then
 * sign-extend when requested. A dword access skips the path entirely: the
 * value is already at width and a 64-bit sign extension would be a no-op
 * that shifts by the type width.
 */
static uint64_t extend(uint64_t value, unsigned nbytes, bool sign_extend) {
  if (nbytes < 8U) {
    const uint64_t mask = (UINT64_C(1) << (nbytes * 8U)) - UINT64_C(1);
    value &= mask;
    if (sign_extend && ((value & (UINT64_C(1) << (nbytes * 8U - 1U))) != 0)) {
      value |= ~mask;
    }
  }
  return value;
}

const oemu_aspace_region *oemu_aspace_internal_find(const oemu_aspace *as, uint64_t pa,
                                                    uint64_t size, uint32_t perms) {
  if ((as == NULL) || (size == 0)) {
    return NULL;
  }
  for (unsigned i = 0; i < as->region_count; ++i) {
    const oemu_aspace_region *r = &as->regions[i];
    /* Contained, without ever forming pa+size (which wraps at the top of
     * the address space). */
    if ((pa >= r->pa) && ((pa - r->pa) <= (r->size - 1U)) &&
        ((r->size - (pa - r->pa)) >= size) && ((r->perms & perms) == perms)) {
      return r;
    }
  }
  return NULL;
}

bool oemu_aspace_internal_ranges_overlap(uint64_t a, uint64_t a_size, uint64_t b,
                                         uint64_t b_size) {
  /* Same no-sum rule as containment: with a <= b the comparison `b - a <
   * a_size` needs no addition and cannot wrap, even when a range ends
   * exactly at the top of the address space. */
  if (a <= b) {
    return (b - a) < a_size;
  }
  return (a - b) < b_size;
}

oemu_status oemu_aspace_internal_check_map_args(uint64_t pa, uint64_t size, uint32_t perms) {
  if ((size == 0) || (perms == 0)) {
    return OEMU_ERR_INVALID_ARG;
  }
  /* Subtract one from the size instead of adding pa+size: a range ending
   * exactly at 2^64 is legal, and the end-exclusive top of the address space
   * must stay mappable. Comparing `size` here would refuse that last byte. */
  if ((size - 1U) > (UINT64_MAX - pa)) {
    return OEMU_ERR_OVERFLOW;
  }
  return OEMU_OK;
}

uint64_t oemu_aspace_internal_disassemble(const uint8_t *src, unsigned nbytes) {
  uint64_t value = 0;
  for (unsigned i = 0; i < nbytes; ++i) {
    value |= (uint64_t)src[i] << (i * 8U);
  }
  return value;
}

void oemu_aspace_internal_assemble(uint8_t *dst, unsigned nbytes, uint64_t value) {
  for (unsigned i = 0; i < nbytes; ++i) {
    dst[i] = (uint8_t)(value >> (i * 8U));
  }
}

/* Shared tail of both mappers: reject overlap and a full table. */
static oemu_status check_room(const oemu_aspace *as, uint64_t pa, uint64_t size) {
  for (unsigned i = 0; i < as->region_count; ++i) {
    if (oemu_aspace_internal_ranges_overlap(pa, size, as->regions[i].pa, as->regions[i].size)) {
      return OEMU_ERR_RANGE;
    }
  }
  if (as->region_count == as->region_capacity) {
    return OEMU_ERR_RANGE; /* the table was sized at init; it does not grow */
  }
  return OEMU_OK;
}

static oemu_status map_common(oemu_aspace *as, uint64_t pa, uint64_t size, uint32_t perms) {
  oemu_status st = oemu_aspace_internal_check_map_args(pa, size, perms);
  if (st != OEMU_OK) {
    return st;
  }
  return check_room(as, pa, size);
}

oemu_status oemu_aspace_init(oemu_aspace *as, unsigned region_capacity) {
  if ((as == NULL) || (region_capacity == 0)) {
    return OEMU_ERR_INVALID_ARG;
  }
  as->regions = NULL;
  as->region_count = 0;
  as->region_capacity = 0;
  const oemu_allocator *alloc = oemu_allocator_get();
  as->allocator = alloc;
  void *table =
      alloc->alloc(sizeof(oemu_aspace_region) * (size_t)region_capacity, alloc->user_data);
  if (table == NULL) {
    as->allocator = NULL;
    return OEMU_ERR_NO_MEMORY;
  }
  as->regions = (oemu_aspace_region *)table;
  as->region_capacity = region_capacity;
  return OEMU_OK;
}

void oemu_aspace_dispose(oemu_aspace *as) {
  if (as == NULL) {
    return;
  }
  if (as->regions != NULL) {
    const oemu_allocator *alloc = as->allocator;
    for (unsigned i = 0; i < as->region_count; ++i) {
      oemu_aspace_region *r = &as->regions[i];
      if ((r->kind == OEMU_REGION_RAM) && r->owned) {
        alloc->free(r->host, alloc->user_data);
      }
    }
    alloc->free(as->regions, alloc->user_data);
  }
  as->regions = NULL;
  as->allocator = NULL;
  as->region_count = 0;
  as->region_capacity = 0;
}

oemu_status oemu_aspace_map_ram(oemu_aspace *as, uint64_t pa, uint64_t size, uint32_t perms,
                                void **host_out) {
  if ((as == NULL) || (as->regions == NULL)) {
    return OEMU_ERR_INVALID_ARG;
  }
  oemu_status st = map_common(as, pa, size, perms);
  if (st != OEMU_OK) {
    return st;
  }
  const oemu_allocator *alloc = as->allocator;
  void *block = alloc->alloc((size_t)size, alloc->user_data);
  if (block == NULL) {
    return OEMU_ERR_NO_MEMORY;
  }
  uint8_t *img = (uint8_t *)block;
  memset(img, 0, (size_t)size); /* a freshly mapped page reads as zeros, not as garbage */
  as->regions[as->region_count] = (oemu_aspace_region){.pa = pa,
                                                       .size = size,
                                                       .perms = perms,
                                                       .kind = OEMU_REGION_RAM,
                                                       .host = img,
                                                       .owned = true};
  ++as->region_count;
  if (host_out != NULL) {
    *host_out = img; /* the caller may be a loader or a test, not the guest */
  }
  return OEMU_OK;
}

oemu_status oemu_aspace_map_ram_alias(oemu_aspace *as, uint64_t pa, void *host, uint64_t size,
                                      uint32_t perms) {
  if ((as == NULL) || (as->regions == NULL) || (host == NULL)) {
    return OEMU_ERR_INVALID_ARG;
  }
  oemu_status st = map_common(as, pa, size, perms);
  if (st != OEMU_OK) {
    return st;
  }
  as->regions[as->region_count] = (oemu_aspace_region){.pa = pa,
                                                       .size = size,
                                                       .perms = perms,
                                                       .kind = OEMU_REGION_RAM,
                                                       .host = (uint8_t *)host,
                                                       .owned = false};
  ++as->region_count;
  return OEMU_OK;
}

oemu_status oemu_aspace_attach_device(oemu_aspace *as, uint64_t pa, uint64_t size,
                                      const oemu_device_ops *ops) {
  if ((as == NULL) || (as->regions == NULL) || (ops == NULL) || (ops->read == NULL) ||
      (ops->write == NULL)) {
    return OEMU_ERR_INVALID_ARG;
  }
  /* A power-of-two size aligned to itself is the device-translation rule
   * every real device tree follows: the offset is then simply the low bits
   * of the address, and misdeployment is caught here rather than in a
   * model's callback. */
  if ((size == 0) || ((size & (size - 1)) != 0) || ((pa % size) != 0)) {
    return OEMU_ERR_INVALID_ARG;
  }
  oemu_status st = map_common(as, pa, size, OEMU_PERM_READ | OEMU_PERM_WRITE);
  if (st != OEMU_OK) {
    return st;
  }
  as->regions[as->region_count] =
      (oemu_aspace_region){.pa = pa,
                           .size = size,
                           .perms = OEMU_PERM_READ | OEMU_PERM_WRITE,
                           .kind = OEMU_REGION_DEVICE,
                           .ops = ops};
  ++as->region_count;
  return OEMU_OK;
}

oemu_status oemu_aspace_validate(const oemu_aspace *as, uint64_t pa, uint64_t size,
                                 uint32_t perms) {
  if ((as == NULL) || (as->regions == NULL) || (size == 0)) {
    return OEMU_ERR_INVALID_ARG;
  }
  /* The bus answers R/W queries for a device region as a whole: whether a
   * particular register exists is the device's call, made at its own
   * read/write callback. */
  return (oemu_aspace_internal_find(as, pa, size, perms) != NULL) ? OEMU_OK : OEMU_ERR_FAULT;
}

/* Resolve an access into a region and its in-region offset. */
static const oemu_aspace_region *resolve(const oemu_aspace *as, uint64_t pa, unsigned nbytes,
                                         uint32_t perms, uint64_t *offset_out) {
  const oemu_aspace_region *r = oemu_aspace_internal_find(as, pa, nbytes, perms);
  if (r != NULL) {
    *offset_out = pa - r->pa;
  }
  return r;
}

oemu_status oemu_aspace_read(const oemu_aspace *as, uint64_t pa, oemu_mem_size size,
                             bool sign_extend, uint64_t *value_out) {
  if ((as == NULL) || (as->regions == NULL) || (value_out == NULL)) {
    return OEMU_ERR_INVALID_ARG;
  }
  const unsigned nbytes = size_to_bytes(size);
  if (nbytes == 0) {
    return OEMU_ERR_INVALID_ARG;
  }
  uint64_t offset = 0;
  const oemu_aspace_region *r = resolve(as, pa, nbytes, OEMU_PERM_READ, &offset);
  if (r == NULL) {
    return OEMU_ERR_FAULT;
  }
  if (r->kind == OEMU_REGION_DEVICE) {
    if ((pa % nbytes) != 0) { /* a device cannot serve a split access */
      return OEMU_ERR_FAULT;
    }
    uint64_t value = 0;
    oemu_status st = r->ops->read(r->ops->ctx, offset, size, &value);
    if (st != OEMU_OK) {
      return st;
    }
    *value_out = extend(value, nbytes, sign_extend);
    return OEMU_OK;
  }
  *value_out =
      extend(oemu_aspace_internal_disassemble(r->host + offset, nbytes), nbytes, sign_extend);
  return OEMU_OK;
}

oemu_status oemu_aspace_write(const oemu_aspace *as, uint64_t pa, oemu_mem_size size,
                              uint64_t value) {
  if ((as == NULL) || (as->regions == NULL)) {
    return OEMU_ERR_INVALID_ARG;
  }
  const unsigned nbytes = size_to_bytes(size);
  if (nbytes == 0) {
    return OEMU_ERR_INVALID_ARG;
  }
  uint64_t offset = 0;
  const oemu_aspace_region *r = resolve(as, pa, nbytes, OEMU_PERM_WRITE, &offset);
  if (r == NULL) {
    return OEMU_ERR_FAULT;
  }
  if (r->kind == OEMU_REGION_DEVICE) {
    if ((pa % nbytes) != 0) {
      return OEMU_ERR_FAULT;
    }
    /* The device sees only the bytes its register width can hold. */
    return r->ops->write(r->ops->ctx, offset, size, extend(value, nbytes, false));
  }
  oemu_aspace_internal_assemble(r->host + offset, nbytes, value);
  return OEMU_OK;
}

oemu_status oemu_aspace_fetch32(const oemu_aspace *as, uint64_t pa, uint32_t *word_out) {
  if ((as == NULL) || (as->regions == NULL) || (word_out == NULL)) {
    return OEMU_ERR_INVALID_ARG;
  }
  /* Instruction fetches are 4-byte aligned: an unaligned fetch cannot be a
   * well-formed instruction. */
  if ((pa % 4) != 0) {
    return OEMU_ERR_INVALID_ARG;
  }
  uint64_t offset = 0;
  const oemu_aspace_region *r = resolve(as, pa, 4, OEMU_PERM_EXEC, &offset);
  if (r == NULL) {
    return OEMU_ERR_FAULT;
  }
  if (r->kind == OEMU_REGION_DEVICE) {
    return OEMU_ERR_FAULT; /* a device is never a code source */
  }
  *word_out = (uint32_t)oemu_aspace_internal_disassemble(r->host + offset, 4);
  return OEMU_OK;
}
