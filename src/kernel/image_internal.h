/*
 * Internal surface of the kernel Image loader: the header bit constants,
 * named so tests assert against meaning instead of hex.
 */
#ifndef OEMU_SRC_KERNEL_IMAGE_INTERNAL_H
#define OEMU_SRC_KERNEL_IMAGE_INTERNAL_H

#include "oemu/image.h"
#include "oemu/macros.h"

OEMU_BEGIN_DECLS

/* flags[7:0] page-size/endianness combinations (booting.rst). */
#define OEMU_IMAGE_FLAG_LE4K  0x01U
#define OEMU_IMAGE_FLAG_LE16K 0x02U
#define OEMU_IMAGE_FLAG_LE64K 0x04U
#define OEMU_IMAGE_FLAG_BE4K  0x08U
#define OEMU_IMAGE_FLAG_BE16K 0x10U
#define OEMU_IMAGE_FLAG_BE64K 0x20U
#define OEMU_IMAGE_FLAG_BE32  0x80U
/* The exact mask the loader rejects on: every big-endian flavour. */
#define OEMU_IMAGE_FLAGS_REJECTED 0xB8U

/* Header offsets, for byte-level test construction. */
#define OEMU_IMAGE_OFF_CODE0       0x00U
#define OEMU_IMAGE_OFF_TEXT_OFFSET 0x08U
#define OEMU_IMAGE_OFF_IMAGE_SIZE  0x10U
#define OEMU_IMAGE_OFF_FLAGS       0x18U
#define OEMU_IMAGE_OFF_MAGIC       0x38U

OEMU_END_DECLS

#endif /* OEMU_SRC_KERNEL_IMAGE_INTERNAL_H */
