/*
 * Internal surface of the kernel Image loader: the header bit constants,
 * named so tests assert against meaning instead of hex.
 *
 * Flag layout per Documentation/arch/arm64/booting.rst (v3.17+ flags field):
 *   bit 0    endianness: 1 = big-endian, 0 = little-endian
 *   bits 1-2 kernel page size: 0 unspecified, 1 = 4K, 2 = 16K, 3 = 64K
 *   bit 3    physical placement hint (low/close-to-DRAM vs within 48-bit range)
 *   bits 4+  reserved
 * oemu is little-endian with a 4K-granule walker, so it rejects bit 0 and a
 * 16K/64K page size; the placement bit is honoured by loading at the base we
 * already use and is otherwise not a reason to refuse.
 */
#ifndef OEMU_SRC_KERNEL_IMAGE_INTERNAL_H
#define OEMU_SRC_KERNEL_IMAGE_INTERNAL_H

#include "oemu/image.h"
#include "oemu/macros.h"

OEMU_BEGIN_DECLS

/* bit 0: the kernel is big-endian; oemu fetches little-endian only. */
#define OEMU_IMAGE_FLAG_ENDIAN_BE 0x01U
/* bits 1-2: page-size selector, and the four values it can hold. */
#define OEMU_IMAGE_FLAG_PAGES_SHIFT 1U
#define OEMU_IMAGE_FLAG_PAGES_MASK  0x06U
#define OEMU_IMAGE_PAGE_UNSPEC      0U
#define OEMU_IMAGE_PAGE_4K          1U
#define OEMU_IMAGE_PAGE_16K         2U
#define OEMU_IMAGE_PAGE_64K         3U
/* bit 3: physical placement hint -- accepted, not a refusal. */
#define OEMU_IMAGE_FLAG_PLACEMENT 0x08U

/* Header offsets, for byte-level test construction (booting.rst order). */
#define OEMU_IMAGE_OFF_CODE0       0x00U
#define OEMU_IMAGE_OFF_CODE1       0x04U
#define OEMU_IMAGE_OFF_TEXT_OFFSET 0x08U
#define OEMU_IMAGE_OFF_IMAGE_SIZE  0x10U
#define OEMU_IMAGE_OFF_FLAGS       0x18U
#define OEMU_IMAGE_OFF_MAGIC       0x38U

/* code0/code1 encodings the loader accepts (see image.c). */
#define OEMU_IMAGE_BRANCH_MASK 0xFC000000U
#define OEMU_IMAGE_BRANCH_BITS 0x14000000U
#define OEMU_IMAGE_BTI_MASK    0xFFFFFEDFU
#define OEMU_IMAGE_BTI_BITS    0xD503201FU

OEMU_END_DECLS

#endif /* OEMU_SRC_KERNEL_IMAGE_INTERNAL_H */
