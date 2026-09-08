/*
 * AArch64 kernel Image loader -- see include/oemu/image.h.
 *
 * The header layout is Documentation/arch/arm64/booting.rst:
 *
 *   0x00 code0 (branch, or a bti pad)   0x20 res1  (8 bytes)
 *   0x04 code1 (branch when code0=bti)  0x28 res2  (8 bytes)
 *   0x08 text_offset (le64)             0x30 res3  (8 bytes)
 *   0x10 image_size (le64)              0x38 magic (le32 "ARM\x64")
 *   0x18 flags     (le64)               0x3C reserved
 *
 * The flags field (D-M4a-6, per booting.rst): bit 0 is endianness (oemu is
 * little-endian, so a big-endian kernel is refused); bits 1-2 are the kernel
 * page size (oemu walks 4K granules, so 16K/64K are refused and 4K/unspecified
 * are accepted); bit 3 is a placement hint we honour by loading at the base we
 * already use. The QEMU oracle admits the same set.
 */
#include "oemu/check.h"

#include <stdbool.h>
#include <string.h>

#include "image_internal.h"

static uint32_t img_le32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) | ((uint32_t)p[2] << 16U) |
         ((uint32_t)p[3] << 24U);
}

static uint64_t img_le64(const unsigned char *p) {
  return (uint64_t)img_le32(p) | ((uint64_t)img_le32(p + 4U) << 32U);
}

oemu_status oemu_image_parse_header(const unsigned char *head, oemu_image *out) {
  if ((head == NULL) || (out == NULL)) {
    return OEMU_ERR_INVALID_ARG;
  }
  /* booting.rst: code0/code1 are responsible for branching to stext. An
   * unannotated Image carries the branch in code0; a BTI-annotated Image
   * (CONFIG_ARM64_BTI_KERNEL) puts a `bti` landing pad in code0 and the
   * branch in code1. Both are legitimate -- accept a branch at code0, or a
   * bti at code0 backed by a branch at code1. The first word being neither
   * is not an Image even if the magic is present. */
  const uint32_t code0 = img_le32(head + OEMU_IMAGE_OFF_CODE0);
  const uint32_t code1 = img_le32(head + OEMU_IMAGE_OFF_CODE1);
  const bool code0_branch = (code0 & OEMU_IMAGE_BRANCH_MASK) == OEMU_IMAGE_BRANCH_BITS;
  const bool code0_bti = (code0 & OEMU_IMAGE_BTI_MASK) == OEMU_IMAGE_BTI_BITS;
  const bool code1_branch = (code1 & OEMU_IMAGE_BRANCH_MASK) == OEMU_IMAGE_BRANCH_BITS;
  if (!code0_branch && !(code0_bti && code1_branch)) {
    return OEMU_ERR_FORMAT;
  }
  if (img_le32(head + 0x38U) != OEMU_IMAGE_MAGIC) {
    return OEMU_ERR_FORMAT;
  }
  /* Endianness is bit 0: a big-endian kernel is refused outright (oemu fetches
   * little-endian). Page size is bits 1-2: only 4K (or the legacy unspecified
   * 0) is served by oemu's walker, so 16K/64K are refused rather than silently
   * mistranslated. The placement bit (3) and reserved bits are not a refusal. */
  const uint64_t flags = img_le64(head + 0x18U);
  if ((flags & OEMU_IMAGE_FLAG_ENDIAN_BE) != 0U) {
    return OEMU_ERR_UNSUPPORTED;
  }
  const uint64_t page_size = (flags >> OEMU_IMAGE_FLAG_PAGES_SHIFT) & 3ULL;
  if ((page_size == OEMU_IMAGE_PAGE_16K) || (page_size == OEMU_IMAGE_PAGE_64K)) {
    return OEMU_ERR_UNSUPPORTED;
  }
  /* booting.rst: text_offset is a 64-bit little-endian doubleword, 4 KiB
   * aligned and under 2 MiB. (A 32-bit read here reads the always-zero high
   * half on a 4K-aligned image and would pass -- wrong for other layouts.) */
  const uint64_t text_offset = img_le64(head + 0x08U);
  const uint64_t image_size = img_le64(head + 0x10U);
  if (((text_offset & 0xFFFU) != 0U) || (text_offset >= 0x200000U)) {
    return OEMU_ERR_FORMAT;
  }
  /* An image_size of zero is the legacy "unknown" encoding; the loader
   * caller supplies the file length in that case, so keep it as-is but
   * an absurd size is caught against the machine, not the header. */
  oemu_image parsed;
  parsed.text_offset = (uint32_t)text_offset; /* validated below 2 MiB, so it fits */
  parsed.image_size = image_size;
  parsed.flags = flags;
  parsed.load_pa = 0U;
  parsed.entry_pa = 0U;
  *out = parsed;
  return OEMU_OK;
}

oemu_status oemu_image_load(oemu_image *out, const unsigned char *bytes, size_t len,
                            const oemu_memops *phys, uint64_t mem_base, uint64_t ram_size) {
  if ((out == NULL) || (bytes == NULL) || (phys == NULL)) {
    return OEMU_ERR_INVALID_ARG;
  }
  OEMU_REQUIRE(phys->write != NULL, "image_load with a half-built bus view");
  if (len < OEMU_IMAGE_HEADER_SIZE) {
    return OEMU_ERR_FORMAT; /* not even a header */
  }
  oemu_image info;
  oemu_status st = oemu_image_parse_header(bytes, &info);
  if (st != OEMU_OK) {
    return st;
  }
  /* The text must lie inside the file and inside the machine. */
  if ((size_t)info.text_offset >= len) {
    return OEMU_ERR_FORMAT;
  }
  const size_t text_len = len - (size_t)info.text_offset;
  if ((uint64_t)info.text_offset + (uint64_t)text_len > ram_size) {
    return OEMU_ERR_RANGE; /* kernel larger than the machine */
  }
  const uint64_t load = mem_base + info.text_offset;
  /* Write it all out. Chunk by natural words where the offset allows
   * (the text_offset contract makes the base 2 MiB-aligned, so every
   * 8-byte chunk is aligned and every bus call is at the widest size);
   * the tail cannot be split from the dword it shares, so it goes out
   * as its containing dword read-then-written back with the file bytes
   * grafted on -- RAM always accepts the read. */
  uint64_t off = 0U;
  for (; off + 8U <= text_len; off += 8U) {
    const uint64_t word = img_le64(bytes + info.text_offset + off);
    const oemu_status w = phys->write(phys->ctx, load + off, OEMU_MEM_DWORD, word);
    if (w != OEMU_OK) {
      return w; /* a bus fault mid-kernel is the caller's machine problem */
    }
  }
  if (off != text_len) {
    const size_t tail = text_len - (size_t)off;
    uint64_t word = 0U;
    (void)memcpy(&word, bytes + info.text_offset + off, tail);
    const oemu_status w = phys->write(phys->ctx, load + off, OEMU_MEM_DWORD, word);
    if (w != OEMU_OK) {
      return w;
    }
  }
  info.load_pa = load;
  info.entry_pa = load; /* booting.rst: enter at the start of text */
  *out = info;
  return OEMU_OK;
}
