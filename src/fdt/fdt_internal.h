/*
 * Internal surface of the FDT module: the wire constants (the format is
 * part of the device-tree specification, pinned here so tests assert on
 * named values) and a read-back helper small enough to audit by eye.
 *
 * The reader exists because a writer tested only by writing proves
 * nothing: every test here reads its expectation back through it, and the
 * boot path re-checks its own DTB the same way before handing it over.
 */
#ifndef OEMU_SRC_FDT_INTERNAL_H
#define OEMU_SRC_FDT_INTERNAL_H

#include "oemu/fdt.h"
#include "oemu/macros.h"
#include "oemu/status.h"

#include <stddef.h>
#include <stdint.h>

OEMU_BEGIN_DECLS

/* Structure-block tokens and the blob magic, per Devicetree Spec v0.2.
 * The token numbers are a classic trap -- verified against `dtc` output
 * byte by byte: BEGIN 1, END_NODE 2, PROP 3 (not 4!), END 9. END_NODE and
 * END carry no payload word; PROP carries name offset, length, and the
 * 4-aligned value inline. */
#define OEMU_FDT_MAGIC            0xD00DFEEDU
#define OEMU_FDT_TOKEN_BEGIN_NODE 0x00000001U
#define OEMU_FDT_TOKEN_END_NODE   0x00000002U
#define OEMU_FDT_TOKEN_PROP       0x00000003U
#define OEMU_FDT_TOKEN_END        0x00000009U
#define OEMU_FDT_V17              17U
/* Node names arrive inline in the structure block; a bound keeps the
 * emit scratch on the stack and the fits() check honest. */
#define OEMU_FDT_MAX_NAME 128U
#define OEMU_FDT_V16      16U

/*
 * Find the value of one property by absolute path ("/pl011@9000000" with
 * the full node name, root as "/"). On success *out points into `blob`
 * (no copy) and *len gives its byte length. Walks linearly: this is for
 * assertions and boot self-check, not a hot path.
 */
OEMU_NODISCARD oemu_status oemu_fdt_internal_find(const unsigned char *blob, size_t blob_len,
                                                  const char *path, const char *prop,
                                                  const unsigned char **out, size_t *len);

/* Total size of a finished blob as the header reports it -- used to prove
 * the header and the emitted bytes agree. */
OEMU_NODISCARD oemu_status oemu_fdt_internal_total_size(const unsigned char *blob,
                                                        size_t blob_len, size_t *out);

OEMU_END_DECLS

#endif /* OEMU_SRC_FDT_INTERNAL_H */
