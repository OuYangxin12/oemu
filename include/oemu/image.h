/*
 * AArch64 kernel Image loader -- the booting.rst protocol.
 *
 * An Image is raw memory, not an ELF: a fixed 64-byte header (branch
 * code, then the metadata) followed by the text at `text_offset`. The
 * loader validates the header against the documented invariants and
 * refuses -- status codes, never a half-load -- anything whose shape it
 * cannot honour: wrong magic, big-endian or unsupported page-size
 * flags, text that would not fit the machine.
 *
 * After a successful load the guest entry point is the physical address
 * the text landed at, and the boot protocol additionally wants x0 = the
 * DTB address and PSTATE = EL1h; the caller (the boot path) arranges
 * those, not the loader.
 */
#ifndef OEMU_IMAGE_H
#define OEMU_IMAGE_H

#include "oemu/macros.h"
#include "oemu/memops.h"
#include "oemu/status.h"

#include <stddef.h>
#include <stdint.h>

OEMU_BEGIN_DECLS

/* Bytes of the Image header every kernel must carry. */
#define OEMU_IMAGE_HEADER_SIZE 64U
/* The header magic at byte offset 0x38: the string "ARM\x64". */
#define OEMU_IMAGE_MAGIC 0x644D5241U

/* The parsed, validated header. */
typedef struct oemu_image {
  uint32_t text_offset; /* offset of the text within the file */
  uint64_t image_size;  /* decompressed image size (bytes) */
  uint64_t flags;       /* header flags, as loaded (validated) */
  uint64_t load_pa;     /* physical address the text was written at */
  uint64_t entry_pa;    /* the instruction pointer to enter at (== load_pa) */
} oemu_image;

/*
 * Parse and validate a 64-byte header without touching memory. `head`
 * must be at least OEMU_IMAGE_HEADER_SIZE bytes. On success *out is
 * filled (load_pa/entry_pa left zero -- parse() knows no machine layout);
 * every refusal is an OEMU_ERR_FORMAT or OEMU_ERR_INVALID_ARG and leaves
 * *out untouched.
 */
OEMU_NODISCARD oemu_status oemu_image_parse_header(const unsigned char *head, oemu_image *out);

/*
 * Load a kernel Image. `bytes` is the file contents (len bytes, host
 * memory), `phys` the guest physical bus, and the text must land at
 * `mem_base + text_offset` inside the RAM window [mem_base,
 * mem_base+ram_size). A file shorter than text_offset+image_size is a
 * truncated Image: refused before anything is written.
 */
OEMU_NODISCARD oemu_status oemu_image_load(oemu_image *out, const unsigned char *bytes,
                                           size_t len, const oemu_memops *phys,
                                           uint64_t mem_base, uint64_t ram_size);

OEMU_END_DECLS

#endif /* OEMU_IMAGE_H */
