/*
 * Internal interface of the ELF module -- NOT part of the public API.
 *
 * Everything here is either a pure byte-level primitive or a pure validation
 * decision, deliberately not `static` so each is exhaustively testable at
 * boundary values with no setup (see AGENTS.md rule 3, and the memory module
 * which this file mirrors). The correctness risks of a loader are narrow and
 * specific -- little-endian reads that must not depend on host byte order,
 * offset arithmetic that must not wrap, and a PT_LOAD whose sizes are internally
 * consistent -- and every one of them lives behind a function below rather than
 * inlined in the load loop where a test could not reach it.
 *
 * Rules for this pattern: never installed, never included by another module's
 * public header, symbols prefixed oemu_elf_internal_.
 */
#ifndef OEMU_SRC_ELF_INTERNAL_H
#define OEMU_SRC_ELF_INTERNAL_H

#include "oemu/macros.h"
#include "oemu/memory.h" /* OEMU_PERM_* for the mapped segment's permissions */
#include "oemu/status.h"

#include <stdbool.h>
#include <stdint.h>

OEMU_BEGIN_DECLS

/*
 * ELF64 constants the loader needs, spelled out rather than pulled from
 * <elf.h>: that header is host/libc-provided, would couple a freestanding-ready
 * library to the host's toolchain, and describes the host's notion of the format
 * rather than the fixed on-disk layout we are contractually bound to.
 */
enum {
  /* e_ident[] sub-indices and the two identity bytes. */
  OEMU_ELF_EI_NIDENT = 16,
  OEMU_ELF_EI_CLASS = 4,
  OEMU_ELF_EI_DATA = 5,
  OEMU_ELF_MAGIC0 = 0x7F, /* the e_ident prefix is 0x7F 'E' 'L' 'F'. */
  OEMU_ELF_MAGIC1 = 'E',
  OEMU_ELF_MAGIC2 = 'L',
  OEMU_ELF_MAGIC3 = 'F',

  /* Values we accept, and the ones we reject as UNSUPPORTED. */
  OEMU_ELF_CLASS64 = 2,       /* EI_CLASS: only 64-bit images. */
  OEMU_ELF_DATA_LSB = 1,      /* EI_DATA: only little-endian. */
  OEMU_ELF_ET_EXEC = 2,       /* e_type: a static, non-relocated executable. */
  OEMU_ELF_EM_AARCH64 = 183U, /* e_machine. */
  OEMU_ELF_PT_LOAD = 1U,      /* p_type: the only program header we act on. */

  /* p_flags bits, mapped onto OEMU_PERM_*. */
  OEMU_ELF_PF_X = 1U,
  OEMU_ELF_PF_W = 2U,
  OEMU_ELF_PF_R = 4U
};

/* Byte offsets of the header and program-header fields we read, so the loader
 * never has to assume a C struct layout for the on-disk image. */
enum {
  OEMU_ELF_OFF_TYPE = 16,
  OEMU_ELF_OFF_MACHINE = 18,
  OEMU_ELF_OFF_ENTRY = 24,
  OEMU_ELF_OFF_PHOFF = 32,
  OEMU_ELF_OFF_PHENTSIZE = 54,
  OEMU_ELF_OFF_PHNUM = 56,

  /* An ELF64 header is 64 bytes; every accepted program-header entry is 56. */
  OEMU_ELF_MIN_HEADER = 64,
  OEMU_ELF_PHENTSIZE = 56
};
enum {
  OEMU_ELF_PH_TYPE = 0,
  OEMU_ELF_PH_FLAGS = 4,
  OEMU_ELF_PH_OFFSET = 8,
  OEMU_ELF_PH_VADDR = 16,
  OEMU_ELF_PH_FILESZ = 32,
  OEMU_ELF_PH_MEMSZ = 40,
  OEMU_ELF_PH_ALIGN = 48
};

/*
 * A validated PT_LOAD, reduced to what oemu_memory_map and
 * oemu_memory_write_bytes need. `memsz` covers the whole mapping (the bss tail
 * is the span past `filesz`); `offset` indexes the borrowed image.
 */
typedef struct oemu_elf_segment {
  uint64_t vaddr;
  uint64_t memsz;
  uint64_t filesz;
  uint64_t offset;
  uint32_t perms;
} oemu_elf_segment;

/*
 * True when the half-open span [off, off+span) lies wholly inside [0, size).
 * Computed as `span <= size - off` after rejecting `off > size`, so it never
 * forms `off + span` and therefore never wraps -- the load-bearing detail that
 * makes a truncated or hostile image fail closed instead of reading out of
 * bounds. A zero span at off == size is in-bounds (reads nothing).
 */
bool oemu_elf_internal_within(uint64_t off, uint64_t span, uint64_t size);

/*
 * Little-endian fixed-width reads, host-endianness-independent, each bounds
 * checked through oemu_elf_internal_within. OEMU_ERR_INVALID_ARG when `img` or
 * `out` is NULL or the span would leave [0, size); OEMU_OK otherwise.
 */
OEMU_NODISCARD oemu_status oemu_elf_internal_rd16(const uint8_t *img, uint64_t size,
                                                  uint64_t off, uint16_t *out);
OEMU_NODISCARD oemu_status oemu_elf_internal_rd32(const uint8_t *img, uint64_t size,
                                                  uint64_t off, uint32_t *out);
OEMU_NODISCARD oemu_status oemu_elf_internal_rd64(const uint8_t *img, uint64_t size,
                                                  uint64_t off, uint64_t *out);

/*
 * Pure decision for one PT_LOAD: turn raw program-header fields into an
 * oemu_elf_segment, or report why the header is unworkable. Returns
 * OEMU_ERR_INVALID_ARG for p_align == 0, no permission bits, filesz > memsz, or
 * a file slice running past `image_size` (or wrapping); OEMU_ERR_OVERFLOW when
 * [vaddr, vaddr+memsz) wraps; OEMU_OK otherwise. `memsz == 0` is INVALID_ARG: a
 * load segment with nothing to map is malformed, and callers skip such entries
 * before ever reaching this.
 */
OEMU_NODISCARD oemu_status oemu_elf_internal_validate_segment(
    uint32_t p_flags, uint64_t p_offset, uint64_t p_vaddr, uint64_t p_filesz, uint64_t p_memsz,
    uint64_t p_align, uint64_t image_size, oemu_elf_segment *out);

/* True when the two half-open ranges [a, a+as) and [b, b+bs) intersect. */
bool oemu_elf_internal_ranges_overlap(uint64_t a, uint64_t as, uint64_t b, uint64_t bs);

OEMU_END_DECLS

#endif /* OEMU_SRC_ELF_INTERNAL_H */
