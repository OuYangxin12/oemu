/*
 * ELF64 loader implementation. See include/oemu/elf.h for the contract and
 * src/elf/elf_internal.h for the pure primitives used here.
 *
 * Shape of oemu_elf_load is "validate, then commit". A guest image is
 * attacker-shaped input and there is no unmap, so every decision that can be
 * made without touching `mem` is made first; only a fully validated set of
 * segments is mapped. That leaves exactly one way to fail midway -- the
 * allocator -- and the header documents the dispose-and-retry it obliges.
 */
#include "oemu/elf.h"

#include "oemu/allocator.h"
#include "oemu/memory.h"
#include "oemu/status.h"

#include <stdint.h>

#include "elf_internal.h"

/* --- pure primitives (see elf_internal.h) ---------------------------------- */

bool oemu_elf_internal_within(uint64_t off, uint64_t span, uint64_t size) {
  if (off > size) {
    return false;
  }
  /* Equivalent to off + span <= size, without ever forming the sum. */
  return span <= (size - off);
}

oemu_status oemu_elf_internal_rd16(const uint8_t *img, uint64_t size, uint64_t off,
                                   uint16_t *out) {
  if ((img == NULL) || (out == NULL) || !oemu_elf_internal_within(off, 2U, size)) {
    return OEMU_ERR_INVALID_ARG;
  }
  *out = (uint16_t)((uint16_t)img[off] | ((uint16_t)img[off + 1U] << 8));
  return OEMU_OK;
}

oemu_status oemu_elf_internal_rd32(const uint8_t *img, uint64_t size, uint64_t off,
                                   uint32_t *out) {
  if ((img == NULL) || (out == NULL) || !oemu_elf_internal_within(off, 4U, size)) {
    return OEMU_ERR_INVALID_ARG;
  }
  *out = (uint32_t)img[off] | ((uint32_t)img[off + 1U] << 8U) |
         ((uint32_t)img[off + 2U] << 16U) | ((uint32_t)img[off + 3U] << 24U);
  return OEMU_OK;
}

oemu_status oemu_elf_internal_rd64(const uint8_t *img, uint64_t size, uint64_t off,
                                   uint64_t *out) {
  if ((img == NULL) || (out == NULL) || !oemu_elf_internal_within(off, 8U, size)) {
    return OEMU_ERR_INVALID_ARG;
  }
  uint64_t value = 0U;
  for (unsigned i = 8U; i-- > 0U;) {
    value = (value << 8U) | (uint64_t)img[off + i];
  }
  *out = value;
  return OEMU_OK;
}

bool oemu_elf_internal_ranges_overlap(uint64_t a, uint64_t as, uint64_t b, uint64_t bs) {
  if ((as == 0U) || (bs == 0U)) {
    return false;
  }
  /* Ranges are validated non-wrapping by the caller, so these sums are safe. */
  return (a < (b + bs)) && (b < (a + as));
}

oemu_status oemu_elf_internal_validate_segment(uint32_t p_flags, uint64_t p_offset,
                                               uint64_t p_vaddr, uint64_t p_filesz,
                                               uint64_t p_memsz, uint64_t p_align,
                                               uint64_t image_size, oemu_elf_segment *out) {
  if (out == NULL) {
    return OEMU_ERR_INVALID_ARG;
  }

  uint32_t perms = 0U;
  perms |= ((p_flags & OEMU_ELF_PF_X) != 0U) ? OEMU_PERM_EXEC : 0U;
  perms |= ((p_flags & OEMU_ELF_PF_W) != 0U) ? OEMU_PERM_WRITE : 0U;
  perms |= ((p_flags & OEMU_ELF_PF_R) != 0U) ? OEMU_PERM_READ : 0U;

  /* p_align of 0 is not a real page size; a segment asking for it is malformed. */
  if ((p_align == 0U) || (perms == 0U) || (p_memsz == 0U)) {
    return OEMU_ERR_INVALID_ARG;
  }
  /* The file slice cannot exceed the mapping, and must sit inside the image.
   * `within` folds the wrap check in, so a huge p_offset is rejected too. */
  if ((p_filesz > p_memsz) || !oemu_elf_internal_within(p_offset, p_filesz, image_size)) {
    return OEMU_ERR_INVALID_ARG;
  }
  if (p_vaddr > (UINT64_MAX - p_memsz)) {
    return OEMU_ERR_OVERFLOW;
  }

  out->vaddr = p_vaddr;
  out->memsz = p_memsz;
  out->filesz = p_filesz;
  out->offset = p_offset;
  out->perms = perms;
  return OEMU_OK;
}

/* --- the loader ------------------------------------------------------------ */

/* Reads one program header's fields (bounds are guaranteed by the caller's
 * whole-table check) and validates them into `seg`. Returns a status the caller
 * propagates verbatim. */
static oemu_status elf_read_segment(const uint8_t *img, uint64_t size, uint64_t base,
                                    uint64_t image_size, oemu_elf_segment *seg) {
  uint32_t p_type = 0U;
  uint32_t p_flags = 0U;
  uint64_t p_offset = 0U;
  uint64_t p_vaddr = 0U;
  uint64_t p_filesz = 0U;
  uint64_t p_memsz = 0U;
  uint64_t p_align = 0U;
  oemu_status status =
      oemu_elf_internal_rd32(img, size, base + (uint64_t)OEMU_ELF_PH_TYPE, &p_type);
  status |= oemu_elf_internal_rd32(img, size, base + (uint64_t)OEMU_ELF_PH_FLAGS, &p_flags);
  status |= oemu_elf_internal_rd64(img, size, base + (uint64_t)OEMU_ELF_PH_OFFSET, &p_offset);
  status |= oemu_elf_internal_rd64(img, size, base + (uint64_t)OEMU_ELF_PH_VADDR, &p_vaddr);
  status |= oemu_elf_internal_rd64(img, size, base + (uint64_t)OEMU_ELF_PH_FILESZ, &p_filesz);
  status |= oemu_elf_internal_rd64(img, size, base + (uint64_t)OEMU_ELF_PH_MEMSZ, &p_memsz);
  status |= oemu_elf_internal_rd64(img, size, base + (uint64_t)OEMU_ELF_PH_ALIGN, &p_align);
  if (status != OEMU_OK) {
    return OEMU_ERR_INVALID_ARG;
  }
  (void)p_type; /* callers filter on type before reaching this */
  return oemu_elf_internal_validate_segment(p_flags, p_offset, p_vaddr, p_filesz, p_memsz,
                                            p_align, image_size, seg);
}

/* True when `base` names a PT_LOAD with a nonzero memory size, i.e. one we map. */
static bool elf_is_loadable(const uint8_t *img, uint64_t size, uint64_t base,
                            uint64_t *memsz_out) {
  uint32_t p_type = 0U;
  uint64_t p_memsz = 0U;
  if (oemu_elf_internal_rd32(img, size, base + (uint64_t)OEMU_ELF_PH_TYPE, &p_type) !=
      OEMU_OK) {
    return false;
  }
  if (p_type != (uint32_t)OEMU_ELF_PT_LOAD) {
    return false;
  }
  if (oemu_elf_internal_rd64(img, size, base + (uint64_t)OEMU_ELF_PH_MEMSZ, &p_memsz) !=
      OEMU_OK) {
    return false;
  }
  if (memsz_out != NULL) {
    *memsz_out = p_memsz;
  }
  return p_memsz != 0U;
}

oemu_status oemu_elf_load(oemu_memory *mem, const void *image, uint64_t size,
                          oemu_elf_image *out) {
  if ((mem == NULL) || (image == NULL) || (out == NULL)) {
    return OEMU_ERR_INVALID_ARG;
  }
  if (size < (uint64_t)OEMU_ELF_MIN_HEADER) {
    return OEMU_ERR_INVALID_ARG;
  }
  const uint8_t *img = (const uint8_t *)image;

  /* Identity: a foreign magic is not an ELF at all. */
  if (((img[0] != (uint8_t)OEMU_ELF_MAGIC0) || (img[1] != (uint8_t)OEMU_ELF_MAGIC1) ||
       (img[2] != (uint8_t)OEMU_ELF_MAGIC2) || (img[3] != (uint8_t)OEMU_ELF_MAGIC3))) {
    return OEMU_ERR_INVALID_ARG;
  }

  /* Format we will not run. Every fixed header field lives within the 64-byte
   * header we already required, so these reads cannot fail; folding them into one
   * status keeps the NODISCARD contract with a single unreachable guard. The
   * big-endian / 32-bit / wrong-machine refusals come before any table math so a
   * foreign image is reported as UNSUPPORTED rather than mis-parsed. */
  uint16_t e_type = 0U;
  uint16_t e_machine = 0U;
  uint64_t e_entry = 0U;
  uint64_t e_phoff = 0U;
  uint16_t e_phentsize = 0U;
  uint16_t e_phnum = 0U;
  oemu_status status = oemu_elf_internal_rd16(img, size, (uint64_t)OEMU_ELF_OFF_TYPE, &e_type);
  status |= oemu_elf_internal_rd16(img, size, (uint64_t)OEMU_ELF_OFF_MACHINE, &e_machine);
  status |= oemu_elf_internal_rd64(img, size, (uint64_t)OEMU_ELF_OFF_ENTRY, &e_entry);
  status |= oemu_elf_internal_rd64(img, size, (uint64_t)OEMU_ELF_OFF_PHOFF, &e_phoff);
  status |= oemu_elf_internal_rd16(img, size, (uint64_t)OEMU_ELF_OFF_PHENTSIZE, &e_phentsize);
  status |= oemu_elf_internal_rd16(img, size, (uint64_t)OEMU_ELF_OFF_PHNUM, &e_phnum);
  if (status != OEMU_OK) {
    return OEMU_ERR_INVALID_ARG;
  }
  if ((img[OEMU_ELF_EI_CLASS] != (uint8_t)OEMU_ELF_CLASS64) ||
      (img[OEMU_ELF_EI_DATA] != (uint8_t)OEMU_ELF_DATA_LSB) ||
      (e_type != (uint16_t)OEMU_ELF_ET_EXEC) || (e_machine != (uint16_t)OEMU_ELF_EM_AARCH64)) {
    return OEMU_ERR_UNSUPPORTED;
  }

  if ((e_phentsize != (uint16_t)OEMU_ELF_PHENTSIZE) || (e_phnum == 0U)) {
    return OEMU_ERR_INVALID_ARG;
  }

  /* The whole program-header table must be present. Computing the bound with an
   * explicit product guard means every later base = e_phoff + i*56 stays in
   * range and its own small offset additions cannot wrap. */
  const uint64_t phnum64 = (uint64_t)e_phnum;
  if (phnum64 > (UINT64_MAX / (uint64_t)OEMU_ELF_PHENTSIZE)) {
    return OEMU_ERR_INVALID_ARG;
  }
  const uint64_t table_bytes = phnum64 * (uint64_t)OEMU_ELF_PHENTSIZE;
  if ((e_phoff > (UINT64_MAX - table_bytes)) || ((e_phoff + table_bytes) > size)) {
    return OEMU_ERR_INVALID_ARG;
  }

  /* Pass 1: how many PT_LOAD segments will actually be mapped. */
  uint64_t load_count = 0U;
  for (uint64_t i = 0U; i < phnum64; i++) {
    const uint64_t base = e_phoff + (i * (uint64_t)OEMU_ELF_PHENTSIZE);
    uint64_t memsz = 0U;
    if (elf_is_loadable(img, size, base, &memsz)) {
      load_count++;
    }
  }
  if (load_count == 0U) {
    return OEMU_ERR_INVALID_ARG; /* a loadable ELF with nothing to load */
  }
  if (((uint64_t)mem->region_count + load_count) > (uint64_t)mem->region_capacity) {
    return OEMU_ERR_RANGE;
  }

  /* Validate all segments into a temporary before mapping anything. The
   * allocation is through the seam so its failure is testable, and it happens
   * before any guest mutation. */
  const oemu_allocator *alloc = oemu_allocator_get();
  const size_t seg_bytes = (size_t)(load_count * (uint64_t)sizeof(oemu_elf_segment));
  oemu_elf_segment *segs = (oemu_elf_segment *)alloc->alloc(seg_bytes, alloc->user_data);
  if (segs == NULL) {
    return OEMU_ERR_NO_MEMORY;
  }

  status = OEMU_OK;
  uint64_t filled = 0U;
  for (uint64_t i = 0U; i < phnum64 && status == OEMU_OK; i++) {
    const uint64_t base = e_phoff + (i * (uint64_t)OEMU_ELF_PHENTSIZE);
    uint64_t memsz = 0U;
    if (!elf_is_loadable(img, size, base, &memsz)) {
      continue;
    }
    status = elf_read_segment(img, size, base, size, &segs[filled]);
    if (status == OEMU_OK) {
      filled++;
    }
  }

  /* Overlap between the segments themselves -- oemu_memory_map would also
   * reject it, but catching it here means a bad image never touches `mem`. */
  for (uint64_t a = 0U; (a < filled) && (status == OEMU_OK); a++) {
    for (uint64_t b = a + 1U; b < filled; b++) {
      if (oemu_elf_internal_ranges_overlap(segs[a].vaddr, segs[a].memsz, segs[b].vaddr,
                                           segs[b].memsz)) {
        status = OEMU_ERR_RANGE;
        break;
      }
    }
  }

  if (status == OEMU_OK) {
    uint64_t load_min = UINT64_MAX;
    uint64_t load_max = 0U;
    for (uint64_t i = 0U; i < filled; i++) {
      const oemu_elf_segment *seg = &segs[i];
      /* One installation per segment: contents copied at map time so a
       * read-only text segment does not need WRITE just to be loaded. */
      const void *src = (seg->filesz > 0U) ? (const void *)(img + seg->offset) : NULL;
      status = oemu_memory_map_image(mem, seg->vaddr, seg->memsz, seg->perms, src, seg->filesz);
      if (status != OEMU_OK) {
        break;
      }
      if (seg->vaddr < load_min) {
        load_min = seg->vaddr;
      }
      const uint64_t seg_top = seg->vaddr + seg->memsz; /* validated non-wrapping */
      if (seg_top > load_max) {
        load_max = seg_top;
      }
    }

    if (status == OEMU_OK) {
      out->entry = e_entry;
      out->load_min = load_min;
      out->load_max = load_max;
      out->segment_count = (uint32_t)filled;
    }
  }

  alloc->free(segs, alloc->user_data);
  return status;
}
