/*
 * FDT (flattened device tree) builder.
 *
 * Emits a device tree blob by appending tokens: begin a node, attach
 * properties, end the node. The buffer is allocated once at init (allocator
 * seam) and the emit functions only append, so a builder can live inside the
 * boot path without ever allocating during emission -- a full buffer is a
 * recoverable OEMU_ERR_NO_MEMORY, not a mid-tree mutation.
 *
 * The result is a spec-conformant binary blob (Devicetree Specification
 * v0.2): the header, the structure block, and the strings block are all
 * produced here; the reservation map is emitted empty. Tests read the
 * result back with the companion reader (oemu/fdt's internal peek or the
 * real `dtc`) rather than trusting the writer alone.
 */
#ifndef OEMU_FDT_H
#define OEMU_FDT_H

#include "oemu/macros.h"
#include "oemu/status.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

OEMU_BEGIN_DECLS

/*
 * An FDT output buffer. `cap` is the ceiling for the finished blob;
 * emission appends without allocating and refuses (OEMU_ERR_NO_MEMORY,
 * object intact and retryable) when the next token would cross it.
 */
typedef struct oemu_fdt {
  unsigned char *data; /* emission scratch, swapped for the blob at finish */
  size_t cap;          /* bytes the finished blob may occupy */
  size_t len;          /* bytes of finished blob; 0 until finish() */
  int depth;           /* open nodes; finish() refuses unbalanced nesting */
  bool finished;       /* finish() closed the tree; emit functions refuse */
  /* Private working state: the structure block appends sequentially (the
   * only order that survives inline names and padded values) while
   * strings accumulate behind it. finish() assembles header, strings,
   * structure and reservation map into one fresh allocation and frees
   * the scratch, so the public blob is exactly cap-bounded and dense. */
  unsigned char *sb; /* structure-block scratch */
  size_t sb_size;    /* bytes of structure scratch allocated */
  size_t sb_used;    /* structure bytes emitted so far */
  size_t str_len;    /* string bytes accumulated so far */
} oemu_fdt;

/* Allocate room for `cap` bytes and open the root node. A root property
 * (compatible, #address-cells, ...) may follow immediately. */
OEMU_NODISCARD oemu_status oemu_fdt_init(oemu_fdt *fdt, size_t cap);

/* Free the buffer and zero the struct. NULL-safe on never-initialized or
 * already-disposed builders. */
void oemu_fdt_dispose(oemu_fdt *fdt);

/* Begin/end one node. `name` may be "" for the root (required once, at
 * depth 0 by begin) or carry a unit address ("memory@40000000"). */
OEMU_NODISCARD oemu_status oemu_fdt_begin_node(oemu_fdt *fdt, const char *name);
OEMU_NODISCARD oemu_status oemu_fdt_end_node(oemu_fdt *fdt);

/* Properties. All must happen inside an open node; a NULL name is a
 * programming error (checked, reported). */
OEMU_NODISCARD oemu_status oemu_fdt_prop_empty(oemu_fdt *fdt, const char *name);
OEMU_NODISCARD oemu_status oemu_fdt_prop_u32(oemu_fdt *fdt, const char *name, uint32_t value);
OEMU_NODISCARD oemu_status oemu_fdt_prop_u64(oemu_fdt *fdt, const char *name, uint64_t value);
OEMU_NODISCARD oemu_status oemu_fdt_prop_str(oemu_fdt *fdt, const char *name,
                                             const char *value);
OEMU_NODISCARD oemu_status oemu_fdt_prop_cells(oemu_fdt *fdt, const char *name,
                                               const uint32_t *cells, size_t count);
/* An opaque property, emitted verbatim and padded to the 4-byte boundary: for
 * byte-defined values such as /chosen/rng-seed, where "big-endian" has no
 * meaning. */
OEMU_NODISCARD oemu_status oemu_fdt_prop_bytes(oemu_fdt *fdt, const char *name,
                                               const void *data, uint32_t len);

/* One string-list property: the elements are concatenated NUL-separated, each
 * terminated, which is the wire form of dtc's `compatible = "a", "b"`. A
 * device node needs it whenever a driver matches on the bus-level fallback
 * (a PL011 that only says "arm,pl011" is invisible to the kernel's AMBA scan,
 * which looks for "arm,primecell"). */
OEMU_NODISCARD oemu_status oemu_fdt_prop_strv(oemu_fdt *fdt, const char *name,
                                              const char *const *values, size_t count);

/* Close the tree (balanced nodes required): completes the structure block,
 * appends the strings block and the empty reservation map, and fixes the
 * header offsets. After this, emit functions refuse; length()/bytes()
 * describe the finished blob. */
OEMU_NODISCARD oemu_status oemu_fdt_finish(oemu_fdt *fdt);

/* Blob bytes and finished length. Before finish() the blob does not
 * exist: bytes() returns NULL and length() 0 -- partial emission is not
 * a wire-valid tree, and only the completed one is offered. */
const unsigned char *oemu_fdt_bytes(const oemu_fdt *fdt);
size_t oemu_fdt_length(const oemu_fdt *fdt);

OEMU_END_DECLS

#endif /* OEMU_FDT_H */
