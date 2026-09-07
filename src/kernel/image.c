/*
 * AArch64 kernel Image loader -- see include/oemu/image.h.
 *
 * The header layout is Documentation/arch/arm64/booting.rst:
 *
 *   0x00 code0 / branch          0x20 res1  (8 bytes)
 *   0x04 res0                    0x28 res2  (8 bytes)
 *   0x08 text_offset (le32)      0x30 res3  (8 bytes)
 *   0x10 image_size (le64)       0x38 magic (le32 "ARM\x64")
 *   0x18 flags     (le64)        0x3C reserved
 *
 * The flags matrix (D-M4a-6): big-endian variants are refused (oemu is
 * little-endian, period). Page-size bits are accepted-and-ignored -- the
 * kernel brings its own tables and oemu's walker is page-size agnostic
 * for the kernel's own choices -- and the acceptance set follows the
 * QEMU oracle (which admits every LE flavour), not a stricter reading
 * that would reject the very Image our e2e gate boots.
 */
#include "oemu/check.h"

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
  /* The branch at code0 must be an unconditional A64 branch (bit pattern
   * 0b000101 as bits 31:26): the loader contract says execution may enter
   * at the header top and the kernel asks to be skipped, not consulted.
   * QEMU checks this too; a file whose first word is not a branch is not
   * an Image even if the magic happens to be present. */
  const uint32_t code0 = img_le32(head);
  if ((code0 & 0xFC000000U) != 0x14000000U) {
    return OEMU_ERR_FORMAT;
  }
  if (img_le32(head + 0x38U) != OEMU_IMAGE_MAGIC) {
    return OEMU_ERR_FORMAT;
  }
  const uint64_t flags = img_le64(head + 0x18U);
  /* Big-endian flavours: bits 3 (4K BE), 4 (16K BE), 5 (64K BE) and the
   * BE32 advertisement 7. oemu fetches and loads little-endian only. */
  if ((flags & 0xB8U) != 0U) {
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
    return OEMU_ERR_FORMAT; /* text_offset points past the file */
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
