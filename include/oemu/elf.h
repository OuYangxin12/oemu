/*
 * ELF64 loader for the AArch64 executor.
 *
 * Maps a static, non-position-independent AArch64 executable into a guest
 * address space and reports where to start running. The scope is deliberately
 * the smallest thing that is honest: the freestanding guest images this project
 * builds (`bench/guest`, linked `-static -nostdlib` at a fixed address) are
 * ET_EXEC with a handful of PT_LOAD segments and no relocations, so the loader
 * validates the file is exactly that and refuses anything else rather than
 * pretending to be a kernel.
 *
 * Nothing here allocates in the guest beyond what the segments need: each PT_LOAD
 * becomes one oemu_memory_map_image (contents installed at map time, zero-filled
 * tail so a .bss needs no separate handling) with the segment's own permissions.
 * The image bytes are borrowed, used only for the duration of the call, and
 * never retained.
 *
 * Parsing reads the file as little-endian bytes at explicit offsets, never as a
 * cast Elf64_* struct: the image has no alignment guarantee (a -Wcast-align and
 * strict-aliasing violation under -Werror) and a byte-level read cannot let the
 * host's byte order silently change what a guest image means.
 */
#ifndef OEMU_ELF_H
#define OEMU_ELF_H

#include "oemu/macros.h"
#include "oemu/memory.h" /* oemu_memory, the destination of a load */
#include "oemu/status.h"

#include <stdint.h>

OEMU_BEGIN_DECLS

/*
 * The result of a successful load. Only meaningful when oemu_elf_load returned
 * OEMU_OK; it is not written on any failure.
 */
typedef struct oemu_elf_image {
  uint64_t entry;         /* e_entry: the initial program counter. */
  uint64_t load_min;      /* lowest guest byte address now mapped by this image. */
  uint64_t load_max;      /* one past the highest; a caller places its stack above this. */
  uint32_t segment_count; /* PT_LOAD segments actually mapped. */
} oemu_elf_image;

/*
 * Validates `image` as an AArch64 ET_EXEC ELF64 and maps every PT_LOAD segment
 * into `mem` with permissions derived from the segment's own flags. `image` and
 * `size` are borrowed and used only during the call (`size` bytes are read from
 * `image`; a null image with a nonzero size is rejected).
 *
 * Returns OEMU_OK when at least one segment was mapped, and then writes `out`.
 * On any other return nothing has been mapped and `out` is left untouched:
 *   OEMU_ERR_INVALID_ARG  a null argument, an image shorter than an ELF64
 *                         header, a bad magic, or a structurally broken program
 *                         header table.
 *   OEMU_ERR_UNSUPPORTED  a well-formed ELF that this loader will not run: not
 *                         64-bit, not little-endian, not AArch64, or not a
 *                         static ET_EXEC (a PIE ET_DYN needs relocations).
 *   OEMU_ERR_OVERFLOW     a segment's or the header table's arithmetic wraps.
 *   OEMU_ERR_RANGE        a PT_LOAD overlaps another mapped range, or `mem` has
 *                         no region-table room for the segments.
 *   OEMU_ERR_NO_MEMORY    a segment's backing block could not be allocated.
 *
 * Every check that does not need to touch the guest runs before the first map,
 * so a malformed image cannot leave `mem` half-loaded. The one failure that can
 * still arrive mid-map is OEMU_ERR_NO_MEMORY (the allocator, not the file);
 * because there is no unmap primitive, callers must then dispose the whole
 * oemu_memory and retry rather than assume it is untouched.
 */
OEMU_NODISCARD oemu_status oemu_elf_load(oemu_memory *mem, const void *image, uint64_t size,
                                         oemu_elf_image *out);

OEMU_END_DECLS

#endif /* OEMU_ELF_H */
