/*
 * A byte-level AArch64 ELF64 image builder for tests.
 *
 * Tests need real ELF bytes -- the loader rejects anything that is not exactly a
 * static ET_EXEC AArch64 image, so a fake struct would test nothing. Assembling
 * images byte-by-byte here (rather than linking with a cross toolchain) means a
 * test failure names the exact header field that caused it, the malformed cases
 * an assembler would never emit are as easy to write as the well-formed ones, and
 * the suite runs on a host with no AArch64 linker at all.
 *
 * Shared by test_elf (the loader's own status contract) and test_cli (feeding a
 * real file to the `oemu run` binary), because duplicating an ELF assembler
 * across two test files is how the two copies quietly disagree.
 *
 * Header-only: everything is inline / inline constexpr so a translation unit that
 * includes but does not use a piece emits nothing for it, and no -Wunused warning
 * fires. Kept out of the library's own headers -- this mirrors the on-disk format
 * independently, so it cannot hide a loader that agrees with itself.
 */
#ifndef OEMU_TESTS_SUPPORT_ELF_BUILDER_H
#define OEMU_TESTS_SUPPORT_ELF_BUILDER_H

#include <cstdint>
#include <vector>

namespace oemu_test {
namespace elf {

/* ELF64 / program-header geometry, mirrored from the on-disk format so the
 * builder can lay bytes down without depending on the library's internals. */
inline constexpr uint64_t kEHdrSize = 64U;
inline constexpr uint64_t kPhdrSize = 56U;
inline constexpr uint64_t kOffType = 16U;
inline constexpr uint64_t kOffMachine = 18U;
inline constexpr uint64_t kOffEntry = 24U;
inline constexpr uint64_t kOffPhoff = 32U;
inline constexpr uint64_t kOffPhentsize = 54U;
inline constexpr uint64_t kOffPhnum = 56U;
inline constexpr uint64_t kOffEiClass = 4U;
inline constexpr uint64_t kOffEiData = 5U;

inline constexpr uint64_t kPhType = 0U;
inline constexpr uint64_t kPhFlags = 4U;
inline constexpr uint64_t kPhOffset = 8U;
inline constexpr uint64_t kPhVaddr = 16U;
inline constexpr uint64_t kPhFilesz = 32U;
inline constexpr uint64_t kPhMemsz = 40U;
inline constexpr uint64_t kPhAlign = 48U;

inline constexpr uint8_t kClass64 = 2U;
inline constexpr uint8_t kDataLsb = 1U;
inline constexpr uint16_t kTypeExec = 2U;
inline constexpr uint16_t kMachineA64 = 183U;
inline constexpr uint32_t kTypeLoad = 1U;
inline constexpr uint32_t kTypeNote = 4U;
inline constexpr uint32_t kFlagRx = 5U; /* PF_R|PF_X */

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

struct SegmentSpec {
  uint64_t vaddr;
  std::vector<uint8_t> data; /* becomes the file slice; filesz = data.size() */
  uint64_t memsz;            /* mapping size; > data.size() adds a zero bss tail */
  uint32_t flags = kFlagRx;
  uint32_t type = kTypeLoad; /* PT_LOAD unless a test exercises a skipped header */
  uint64_t align = 0x1000U;
};

/* A 64-byte ELF64 header with an empty program-header table. */
inline std::vector<uint8_t> bare_header(uint16_t phnum) {
  std::vector<uint8_t> v(kEHdrSize, 0U);
  v[0] = 0x7FU;
  v[1] = 'E';
  v[2] = 'L';
  v[3] = 'F';
  v[kOffEiClass] = kClass64;
  v[kOffEiData] = kDataLsb;
  put16(v, kOffType, kTypeExec);
  put16(v, kOffMachine, kMachineA64);
  put64(v, kOffEntry, 0U);
  put64(v, kOffPhoff, kEHdrSize);
  put16(v, kOffPhentsize, (uint16_t)kPhdrSize);
  put16(v, kOffPhnum, phnum);
  return v;
}

/* Assembles a well-formed image: header, `segs.size()` program-header entries at
 * phoff=64, then the payloads. memsz may exceed the payload size to model .bss. */
inline std::vector<uint8_t> build_image(const std::vector<SegmentSpec> &segs, uint64_t entry) {
  const uint64_t phnum = (uint64_t)segs.size();
  const uint64_t data_start = kEHdrSize + phnum * kPhdrSize;
  std::vector<uint8_t> v = bare_header((uint16_t)phnum);
  v.resize(data_start, 0U);

  uint64_t cursor = data_start;
  std::vector<uint64_t> offsets;
  for (const SegmentSpec &s : segs) {
    offsets.push_back(cursor);
    v.insert(v.end(), s.data.begin(), s.data.end());
    cursor += s.data.size();
  }

  put64(v, kOffEntry, entry);
  for (uint64_t i = 0U; i < phnum; i++) {
    const SegmentSpec &s = segs[i];
    const uint64_t ph = kEHdrSize + i * kPhdrSize;
    put32(v, ph + kPhType, s.type);
    put32(v, ph + kPhFlags, s.flags);
    put64(v, ph + kPhOffset, offsets[i]);
    put64(v, ph + kPhVaddr, s.vaddr);
    put64(v, ph + kPhFilesz, (uint64_t)s.data.size());
    put64(v, ph + kPhMemsz, s.memsz);
    put64(v, ph + kPhAlign, s.align);
  }
  return v;
}

/* Little-endian 32-bit words -> file-order bytes. */
inline std::vector<uint8_t> to_bytes(const std::vector<uint32_t> &words) {
  std::vector<uint8_t> out;
  for (uint32_t w : words) {
    for (unsigned i = 0U; i < 4U; i++) {
      out.push_back((uint8_t)((w >> (8U * i)) & 0xFFU));
    }
  }
  return out;
}

}  // namespace elf
}  // namespace oemu_test

#endif /* OEMU_TESTS_SUPPORT_ELF_BUILDER_H */
