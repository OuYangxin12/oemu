/*
 * tests/support/page_table_builder.h -- independent AArch64 stage-1 page-table
 * builder for tests.
 *
 * Mirrors the VMSAv8-64 (EL1&0, 4 KiB granule) descriptor formats so M3a tests
 * can hand page tables into emulated RAM without importing a single definition
 * from the future src/mmu/ walker -- the elf_builder contract: a builder that
 * included the loader's own definitions could only prove the loader agrees with
 * itself. Format knowledge here comes from the architecture, cited inline, and
 * is cross-checked against the Linux 6.6 tree in guest/src/ (the same external
 * references tests/README.md names for independent oracles).
 *
 * Citations: Arm DDI 0487J "VMSAv8-64 translation table format descriptors"
 * and "AArch64 Translation Table Base registers"; Linux 6.6
 * arch/arm64/include/asm/pgtable-hwdef.h (PTE_* / TCR_* constants, quoted in
 * the comments below). The M3a walk math this encodes: with a 4 KiB granule
 * each level indexes 9 bits and the page offset is 12 bits, so a translation
 * starting at level L covers (4-L)*9+12 VA bits; TCR.TnSZ=25 (39-bit VA, the
 * tinyconfig baseline) therefore starts at L1 and TnSZ=16 (48-bit) at L0.
 *
 * Pure host-side code: no allocation (every buffer is caller-provided), no
 * oemu includes, no host endianness assumptions (all descriptor words are
 * emitted as explicit little-endian bytes). Guest-visible RAM gets the image
 * via oemu_aspace_map_ram_alias at the image base, so a walker reading through
 * memops sees exactly the bytes built here.
 */
#ifndef OEMU_TESTS_PAGE_TABLE_BUILDER_H
#define OEMU_TESTS_PAGE_TABLE_BUILDER_H

#include <cstddef>
#include <cstdint>

namespace oemu_test {

/* --- geometry ------------------------------------------------------------ */

inline constexpr uint64_t kPageSize = 4096;
inline constexpr int kEntriesPerTable = 512;
inline constexpr uint64_t kTableBytes = kPageSize;
inline constexpr int kPageShift = 12;
/* Bits indexed by one lookup level: 512 entries x 8 bytes = one 4 KiB table. */
inline constexpr int kIndexBits = 9;
/*
 * Block size per ARM level, indexed BY LEVEL: kBlockBytes[L] is the granule
 * of a block descriptor living in a level-L table (L1: 1 GiB, L2: 2 MiB,
 * L3: 4 KiB). kBlockBytes[0] is 0 because L0 has no block form -- tables only.
 */
inline constexpr uint64_t kBlockBytes[4] = {0, 1ULL << 30, 1ULL << 21, 1ULL << 12};

/*
 * TCR.TnSZ -> root (start) level for a 4 KiB granule. The walk starts at the
 * highest level L whose covered bit count (4-L)*9+12 reaches the VA width,
 * equivalently the smallest L with TnSZ >= 64 - ((4-L)*9+12). Thresholds:
 *   [16..24] -> L0, [25..33] -> L1, [34..42] -> L2, [43..48] -> L3.
 * Anchors: TnSZ=16 (48-bit VA) -> L0; TnSZ=25 (39-bit VA, tinyconfig) -> L1.
 * Returns -1 outside the legal [16, 48] range.
 */
inline int root_level_for_tnsz(uint64_t tnsz) {
  if (tnsz < 16 || tnsz > 48) {
    return -1;
  }
  if (tnsz >= 43) {
    return 3;
  }
  if (tnsz >= 34) {
    return 2;
  }
  if (tnsz >= 25) {
    return 1;
  }
  return 0;
}

/* VA width -> TCR.TnSZ (T0SZ/T1SZ field value). Must be in [16, 48]. */
inline uint64_t tnsz_for_va_bits(uint64_t va_bits) {
  return 64 - va_bits;
}

/* Table entry index for `va` at `level` (0..3): VA[12+9*(3-L)+8 : 12+9*(3-L)]. */
inline int entry_index(int level, uint64_t va) {
  const int shift = kPageShift + kIndexBits * (3 - level);
  return static_cast<int>((va >> shift) & static_cast<uint64_t>(kEntriesPerTable - 1));
}

/* --- descriptor encodings ------------------------------------------------ */

/* bits[1:0] (hwdef.h PTE_VALID bit 0, PTE_TABLE_BIT bit 1). */
inline constexpr uint64_t kDescInvalid = 0x0; /* bit0 = 0: fault on use */
inline constexpr uint64_t kDescBlock = 0x1;   /* L1/L2 block: valid + !table */
inline constexpr uint64_t kDescTable = 0x3;   /* L0-L2 table / L3 page: 11 */

/* Lower attributes, bits[11:2] of block/page descriptors. */
inline constexpr int kAttrIndxShift = 2; /* AttrIndx[2] bits[4:2] (PTE_ATTRINDX) */
inline constexpr uint64_t kAttrIndxMask = 0x7;
inline constexpr int kNsShift = 5;             /* NS, bit 5 */
inline constexpr int kApShift = 6;             /* AP[2:1] bits[7:6] */
inline constexpr int kShShift = 8;             /* SH[1:0] bits[9:8] */
inline constexpr uint64_t kAfBit = 1ULL << 10; /* AF (PTE_AF) */
inline constexpr uint64_t kNgBit = 1ULL << 11; /* nG (PTE_NG) */
inline constexpr uint64_t kContiguousBit = 1ULL << 52;
inline constexpr uint64_t kPxnBit = 1ULL << 53; /* PXN (PTE_PXN) */
inline constexpr uint64_t kUxnBit = 1ULL << 54; /* UXN (PTE_UXN) */

/*
 * AP[2:1]: AP[1] (bit 6) enables EL0 access (hwdef.h PTE_USER), AP[2]
 * (bit 7) marks read-only (PTE_RDONLY) -- the same two gates the kernel
 * encodes, cited here as the external reference.
 */
enum class Ap : uint64_t {
  El1Rw = 0b00,  /* RW, no EL0 access */
  El01Rw = 0b01, /* RW, EL0 and EL1 */
  El1Ro = 0b10,  /* read-only, no EL0 */
  El01Ro = 0b11, /* read-only, EL0 and EL1 */
};

enum class Shareable : uint64_t {
  None = 0b00,  /* non-shareable (devices) */
  Outer = 0b10, /* outer shareable */
  Inner = 0b11, /* inner shareable (normal cached memory) */
};

/*
 * MAIR attribute bytes (DDI 0487J "MAIR_EL1 attribute encodings"; Linux
 * MAIR_ATTR_*): device flavors carry bits[3:2], normal memory is
 * 0bRRRR_IIII outer/inner.
 */
inline constexpr uint8_t kMairDeviceNgnrne = 0x00;
inline constexpr uint8_t kMairDeviceNgnre = 0x04;
inline constexpr uint8_t kMairNormalNc = 0x44;
inline constexpr uint8_t kMairNormalWb = 0xFF;

/* One MAIR attribute slot (0..7) of MAIR_EL1. */
inline constexpr uint64_t mair_attr(int index, uint8_t attr) {
  return static_cast<uint64_t>(attr) << (8 * index);
}

/* Assemble bits[11:2] of a block/page descriptor. */
inline constexpr uint64_t lower_attrs(int attr_indx, Ap ap, Shareable sh, bool af, bool ng) {
  return (static_cast<uint64_t>(attr_indx & 0x7) << kAttrIndxShift) |
         (static_cast<uint64_t>(ap) << kApShift) | (static_cast<uint64_t>(sh) << kShShift) |
         (af ? kAfBit : 0) | (ng ? kNgBit : 0);
}

/*
 * Table descriptor (L0-L2): next-table PA in bits[47:12] plus the inheritance
 * bits a walker must accumulate: NSTable[63], APTable[62:61], PXNTable[60],
 * UXNTable[59]. APTable 0b01 removes EL0 access below, 0b10 forces read-only,
 * 0b11 both (DDI 0487J "table descriptor format").
 */
inline constexpr uint64_t table_desc(uint64_t next_table_pa, bool ns_table = false,
                                     int ap_table = 0, bool pxn_table = false,
                                     bool uxn_table = false) {
  return kDescTable | (next_table_pa & ~static_cast<uint64_t>(kPageSize - 1)) |
         (ns_table ? (1ULL << 63) : 0) | (static_cast<uint64_t>(ap_table & 0x3) << 61) |
         (pxn_table ? (1ULL << 60) : 0) | (uxn_table ? (1ULL << 59) : 0);
}

/* Block descriptor (L1: 1 GiB, L2: 2 MiB); PA must be block-aligned. */
inline constexpr uint64_t block_desc(uint64_t pa, uint64_t lower, bool pxn = false,
                                     bool uxn = false, bool contiguous = false) {
  return kDescBlock | (pa & ~static_cast<uint64_t>(kPageSize - 1)) | lower |
         (pxn ? kPxnBit : 0) | (uxn ? kUxnBit : 0) | (contiguous ? kContiguousBit : 0);
}

/* Page descriptor (L3, 4 KiB); PA must be page-aligned. */
inline constexpr uint64_t page_desc(uint64_t pa, uint64_t lower, bool pxn = false,
                                    bool uxn = false, bool contiguous = false) {
  return kDescTable | (pa & ~static_cast<uint64_t>(kPageSize - 1)) | lower |
         (pxn ? kPxnBit : 0) | (uxn ? kUxnBit : 0) | (contiguous ? kContiguousBit : 0);
}

/* --- the physical image -------------------------------------------------- */

enum class PtbStatus {
  kOk,
  kOutOfRange, /* PA range falls outside the image */
  kMisaligned, /* PA or VA not aligned for the requested granularity */
  kNoRoom,     /* the image cursor has no space for another table */
};

/*
 * A caller-owned host buffer standing in for guest physical memory
 * [base, base + size). All builders below are pure functions over it.
 */
struct PhysImage {
  uint8_t *data = nullptr;
  uint64_t size = 0;
  uint64_t base = 0;

  /* Bounds-checked pointer at `pa`; NULL when the range leaves the image. */
  uint8_t *at(uint64_t pa, uint64_t bytes) const {
    if (pa < base || bytes > size || pa - base > size - bytes) {
      return nullptr;
    }
    return data + (pa - base);
  }
};

/* Each descriptor slot is 8 bytes (u64), little-endian on the guest bus. */
inline constexpr uint64_t kDescBytes = 8;

/* Little-endian 64-bit descriptor access (guest-visible byte order). */
inline uint64_t desc_read(const PhysImage &img, uint64_t table_pa, int index) {
  const uint8_t *p = img.at(table_pa + static_cast<uint64_t>(index) * kDescBytes, kDescBytes);
  if (p == nullptr) {
    return 0;
  }
  return static_cast<uint64_t>(p[0]) | (static_cast<uint64_t>(p[1]) << 8) |
         (static_cast<uint64_t>(p[2]) << 16) | (static_cast<uint64_t>(p[3]) << 24) |
         (static_cast<uint64_t>(p[4]) << 32) | (static_cast<uint64_t>(p[5]) << 40) |
         (static_cast<uint64_t>(p[6]) << 48) | (static_cast<uint64_t>(p[7]) << 56);
}

inline void desc_write(PhysImage &img, uint64_t table_pa, int index, uint64_t word) {
  uint8_t *p = img.at(table_pa + static_cast<uint64_t>(index) * kDescBytes, kDescBytes);
  if (p == nullptr) {
    return;
  }
  for (int i = 0; i < 8; i++) {
    p[i] = static_cast<uint8_t>((word >> (8 * i)) & 0xFF);
  }
}

/* Zero a 4 KiB table: every entry invalid, the walk faults on use. */
inline PtbStatus table_clear(PhysImage &img, uint64_t table_pa) {
  if (img.at(table_pa, kTableBytes) == nullptr) {
    return PtbStatus::kOutOfRange;
  }
  uint8_t *p = img.at(table_pa, kTableBytes);
  for (uint64_t i = 0; i < kTableBytes; i++) {
    p[i] = 0;
  }
  return PtbStatus::kOk;
}

/*
 * Monotonic allocator of 4 KiB table slots from the image cursor. Tests place
 * the root explicitly and let intermediate tables come from here; the layout
 * is deterministic, so a test can name every table PA it will observe.
 */
struct TableArena {
  PhysImage *img = nullptr;
  uint64_t cursor = 0; /* next free table PA */
};

inline PtbStatus arena_init(TableArena &arena, PhysImage &img, uint64_t first_free_pa) {
  if (img.at(first_free_pa, kTableBytes) == nullptr) {
    return PtbStatus::kOutOfRange;
  }
  arena.img = &img;
  arena.cursor = first_free_pa;
  return PtbStatus::kOk;
}

inline PtbStatus arena_take(TableArena &arena, uint64_t *table_pa_out) {
  if (arena.img->at(arena.cursor, kTableBytes) == nullptr) {
    return PtbStatus::kNoRoom;
  }
  PtbStatus st = table_clear(*arena.img, arena.cursor);
  if (st != PtbStatus::kOk) {
    return st;
  }
  *table_pa_out = arena.cursor;
  arena.cursor += kTableBytes;
  return PtbStatus::kOk;
}

/*
 * Map [va, va + size) -> [pa, pa + size) with leaf descriptors at `leaf_level`
 * (1 = 1 GiB blocks, 2 = 2 MiB blocks, 3 = 4 KiB pages), creating intermediate
 * tables from `arena` as needed. `root_pa`/`root_level` name the root table.
 * Requires va, pa, and size aligned to the leaf granularity.
 */
inline PtbStatus map_range(PhysImage &img, TableArena &arena, uint64_t root_pa, int root_level,
                           uint64_t va, uint64_t pa, uint64_t size, int leaf_level,
                           uint64_t lower, bool pxn = false, bool uxn = false) {
  /* L0 has no block form (tables only), and nothing above L3 exists.
   * Validate before indexing kBlockBytes: UBSan rightly traps the
   * out-of-bounds read that a leaf_level == 4 call would otherwise do. */
  if (leaf_level < 1 || leaf_level > 3 || leaf_level < root_level) {
    return PtbStatus::kMisaligned;
  }
  const uint64_t gran = kBlockBytes[leaf_level];
  if ((va & (gran - 1)) != 0 || (pa & (gran - 1)) != 0 || (size & (gran - 1)) != 0) {
    return PtbStatus::kMisaligned;
  }
  for (uint64_t off = 0; off < size; off += gran) {
    uint64_t table_pa = root_pa;
    for (int level = root_level; level < leaf_level; level++) {
      const int index = entry_index(level, va + off);
      uint64_t word = desc_read(img, table_pa, index);
      if ((word & 0x3) != kDescTable) {
        if (word != 0) {
          return PtbStatus::kMisaligned; /* a block already covers this range */
        }
        uint64_t next_pa = 0;
        PtbStatus st = arena_take(arena, &next_pa);
        if (st != PtbStatus::kOk) {
          return st;
        }
        word = table_desc(next_pa);
        desc_write(img, table_pa, index, word);
      }
      table_pa = word & ~static_cast<uint64_t>(kPageSize - 1);
    }
    const int leaf_index = entry_index(leaf_level, va + off);
    uint64_t word = desc_read(img, table_pa, leaf_index);
    if (word != 0) {
      return PtbStatus::kMisaligned; /* re-map would silently shadow */
    }
    const uint64_t leaf = (leaf_level == 3) ? page_desc(pa + off, lower, pxn, uxn)
                                            : block_desc(pa + off, lower, pxn, uxn);
    desc_write(img, table_pa, leaf_index, leaf);
  }
  return PtbStatus::kOk;
}

} /* namespace oemu_test */

#endif /* OEMU_TESTS_PAGE_TABLE_BUILDER_H */
