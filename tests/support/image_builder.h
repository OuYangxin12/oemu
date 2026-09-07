/*
 * A byte-level AArch64 kernel Image builder for tests.
 *
 * The Image loader validates a fixed 64-byte header against the invariants
 * in Documentation/arch/arm64/booting.rst and refuses anything whose shape
 * it cannot honour. A fake struct would test nothing, so tests lay the
 * header down as raw little-endian bytes exactly as booting.rst numbers it:
 * the branch at code0, the metadata at its documented offset, the magic at
 * 0x38. That way a failure names the field, the malformed cases an
 * assembler would never emit are trivial to write, and the suite runs with
 * no AArch64 toolchain at all.
 *
 * Header-only: every piece is inline so an unused include emits nothing and
 * no -Wunused warning fires. It mirrors the on-disk format independently of
 * src/kernel, so it cannot hide a loader that agrees only with itself.
 */
#ifndef OEMU_TESTS_SUPPORT_IMAGE_BUILDER_H
#define OEMU_TESTS_SUPPORT_IMAGE_BUILDER_H

#include <cstdint>
#include <vector>

namespace oemu_test {
namespace image {

/* booting.rst geometry: mirrored from the on-disk format, not from src/. */
inline constexpr uint64_t kHeaderSize = 64U;
inline constexpr uint64_t kOffCode0 = 0x00U;
inline constexpr uint64_t kOffTextOffset = 0x08U;
inline constexpr uint64_t kOffImageSize = 0x10U;
inline constexpr uint64_t kOffFlags = 0x18U;
inline constexpr uint64_t kOffMagic = 0x38U;

inline constexpr uint32_t kMagic = 0x644D5241U; /* "ARM\x64" */
/* code0: b . + <text_offset>, the unconditional-branch the loader demands. */
inline constexpr uint32_t kBranchTo0x80000 = 0x14020000U;

/* The page-size / endianness flag bits (booting.rst flags[7:0]). */
inline constexpr uint64_t kFlagLe4K = 0x01U;
inline constexpr uint64_t kFlagLe16K = 0x02U;
inline constexpr uint64_t kFlagLe64K = 0x04U;
inline constexpr uint64_t kFlagBe4K = 0x08U;
inline constexpr uint64_t kFlagBe16K = 0x10U;
inline constexpr uint64_t kFlagBe64K = 0x20U;
inline constexpr uint64_t kFlagBe32 = 0x80U;

inline void put16(std::vector<uint8_t> &v, uint64_t off, uint16_t value) {
  v[off] = (uint8_t)(value & 0xFFU);
  v[off + 1U] = (uint8_t)((value >> 8) & 0xFFU);
}
inline void put32(std::vector<uint8_t> &v, uint64_t off, uint32_t value) {
  for (unsigned i = 0U; i < 4U; i++) {
    v[off + i] = (uint8_t)((value >> (8U * i)) & 0xFFU);
  }
}
inline void put64(std::vector<uint8_t> &v, uint64_t off, uint64_t value) {
  for (unsigned i = 0U; i < 8U; i++) {
    v[off + i] = (uint8_t)((value >> (8U * i)) & 0xFFU);
  }
}

/* A single unconditional A64 branch (bits 31:26 == 0b000101) to the given
 * PC-relative byte offset. The loader accepts only this shape at code0. */
inline uint32_t branch_to(uint64_t byte_offset) {
  const uint32_t imm26 = (uint32_t)((byte_offset / 4U) & 0x03FFFFFFU);
  return 0x14000000U | imm26;
}

/* A 64-byte all-zero header: no branch, no magic -- the null case to poke. */
inline std::vector<uint8_t> blank(void) {
  return std::vector<uint8_t>(kHeaderSize, 0U);
}

/* A fully well-formed 64-byte header for the given layout. `image_size` is
 * the on-disk size declared by the file, `flags` the page/endianness bits. */
inline std::vector<uint8_t> header(uint32_t text_offset, uint64_t image_size, uint64_t flags) {
  std::vector<uint8_t> v = blank();
  put32(v, kOffCode0, branch_to(text_offset));
  put64(v, kOffTextOffset, text_offset);
  put64(v, kOffImageSize, image_size);
  put64(v, kOffFlags, flags);
  put32(v, kOffMagic, kMagic);
  return v;
}

/*
 * A complete Image file: the header, zero padding up to `text_offset`, then
 * `text`. The materialised hole is what makes the file honest -- text_offset
 * points at real bytes. image_size in the header is the whole file length.
 */
inline std::vector<uint8_t> file(uint32_t text_offset, const std::vector<uint8_t> &text,
                                 uint64_t flags = kFlagLe4K) {
  std::vector<uint8_t> v = header(text_offset, (uint64_t)(text_offset + text.size()), flags);
  if (v.size() < text_offset) {
    v.resize(text_offset, 0U); /* materialise the hole, never truncate the header */
  }
  v.insert(v.end(), text.begin(), text.end());
  return v;
}

/* An obviously-executable text word: an unconditional branch to itself, the
 * classic park. A loader never executes it, so any A64 word would do. */
inline std::vector<uint8_t> park_text(size_t words = 1U) {
  std::vector<uint8_t> t;
  /* "b ." = 0x14000000, little-endian on the bus: 00 00 00 14. */
  for (size_t i = 0U; i < words; i++) {
    t.push_back(0x00U);
    t.push_back(0x00U);
    t.push_back(0x00U);
    t.push_back(0x14U);
  }
  return t;
}

}  // namespace image
}  // namespace oemu_test

#endif /* OEMU_TESTS_SUPPORT_IMAGE_BUILDER_H */
