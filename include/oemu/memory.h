/*
 * Flat-address-space guest memory for the executor.
 *
 * The model is a region table rather than a page table: the emulated subset is
 * user-mode and single-threaded, so a handful of mapped ranges (guest text,
 * data, a stack) is all the structure that is needed, and a lookup that walks
 * ten entries beats one that walks a radix tree.
 *
 * Regions never overlap and never grow. The table has a fixed capacity chosen
 * at init, so no operation on a live memory model can allocate behind the
 * caller's back -- except oemu_memory_map itself, which takes exactly one
 * backing allocation per mapped region through the allocator seam.
 *
 * All guest-visible access is little-endian and byte-assembled, so the host's
 * own byte order never leaks into guest state.
 */
#ifndef OEMU_MEMORY_H
#define OEMU_MEMORY_H

#include "oemu/allocator.h"
#include "oemu/decode.h" /* oemu_mem_size: the transfer-size selector */
#include "oemu/macros.h"
#include "oemu/memops.h" /* the bus view returned by oemu_memory_memops */
#include "oemu/status.h"

#include <stdbool.h>
#include <stdint.h>

OEMU_BEGIN_DECLS

/* Region permission bits. */
#define OEMU_PERM_READ  ((uint32_t)0x1U)
#define OEMU_PERM_WRITE ((uint32_t)0x2U)
#define OEMU_PERM_EXEC  ((uint32_t)0x4U)
#define OEMU_PERM_ALL   (OEMU_PERM_READ | OEMU_PERM_WRITE | OEMU_PERM_EXEC)

/*
 * Internal record type; defined in src/memory/memory_internal.h. Declaring it
 * here lets the struct below hold a pointer to it while the public header stays
 * free of layout details that a test must not depend on.
 */
struct oemu_memory_region;

/*
 * The struct is not opaque so it can live on the stack; treat the fields as
 * read-only and go through the functions. Exactly two allocations exist per
 * instance (the region table, plus one backing block per oemu_memory_map
 * region), which is what makes the OOM paths exhaustively testable.
 */
typedef struct oemu_memory {
  struct oemu_memory_region *regions;
  const oemu_allocator *allocator;
  unsigned region_count;
  unsigned region_capacity;
} oemu_memory;

/*
 * Empties the model with room for `region_capacity` regions. Allocates the
 * region table. Returns OEMU_ERR_INVALID_ARG on NULL or a zero capacity,
 * OEMU_ERR_NO_MEMORY if the table cannot be allocated.
 */
OEMU_NODISCARD oemu_status oemu_memory_init(oemu_memory *mem, unsigned region_capacity);

/* Frees the region table and every owned backing block. Safe to call twice; the
 * struct is zeroed afterwards and must be re-inited before further use. */
void oemu_memory_dispose(oemu_memory *mem);

/*
 * Maps [gva, gva+size) freshly allocated, zero-filled memory with `perms`.
 * OEMU_ERR_INVALID_ARG: NULL model, zero size, or zero perms.
 * OEMU_ERR_OVERFLOW: the range wraps past 2^64.
 * OEMU_ERR_RANGE: overlaps an existing region, or the table is full.
 * OEMU_ERR_NO_MEMORY: the backing block cannot be allocated.
 */
OEMU_NODISCARD oemu_status oemu_memory_map(oemu_memory *mem, uint64_t gva, uint64_t size,
                                           uint32_t perms);

/*
 * Maps a caller-owned host buffer at `gva`. Nothing is allocated and nothing is
 * copied; the buffer must outlive the mapping. This is how tests place guest
 * code without going through the allocator, and how a future loader can adopt
 * an existing buffer. Same argument errors as oemu_memory_map, plus
 * OEMU_ERR_INVALID_ARG for a NULL host.
 */
OEMU_NODISCARD oemu_status oemu_memory_map_alias(oemu_memory *mem, uint64_t gva, void *host,
                                                 uint64_t size, uint32_t perms);

/*
 * Reports whether [gva, gva+size) lies wholly inside one region carrying at
 * least `perms`, without reading or writing anything. The executor uses this to
 * stage multi-register accesses (STP, LDP writeback) so that a fault on the
 * second transfer cannot commit the first.
 *
 * OEMU_ERR_FAULT on unmapped, cross-region or under-permissioned.
 * OEMU_ERR_INVALID_ARG for a NULL model or a zero size.
 */
OEMU_NODISCARD oemu_status oemu_memory_validate(const oemu_memory *mem, uint64_t gva,
                                                uint64_t size, uint32_t perms);

/*
 * Reads `size` little-endian bytes from `gva`, optionally sign-extending the
 * loaded value to 64 bits. OEMU_ERR_FAULT when oemu_memory_validate would fail.
 */
OEMU_NODISCARD oemu_status oemu_memory_read(const oemu_memory *mem, uint64_t gva,
                                            oemu_mem_size size, bool sign_extend,
                                            uint64_t *value_out);

/* Writes the low (1 << size) little-endian bytes of `value` to `gva`. */
OEMU_NODISCARD oemu_status oemu_memory_write(const oemu_memory *mem, uint64_t gva,
                                             oemu_mem_size size, uint64_t value);

/*
 * Maps an owned, zero-filled region exactly like oemu_memory_map, but installs
 * `contents` (of `contents_size <= size` bytes) at its front before the region
 * becomes reachable. This is the file-backed-segment primitive a loader needs:
 * the copy happens at installation time rather than as a guest store, so it
 * succeeds even when the region's final `perms` omit WRITE -- which
 * oemu_memory_write_bytes would reject for read-only text. The span past
 * `contents_size` stays zero, so a .bss tail needs no separate handling.
 *
 * Same errors as oemu_memory_map for the region itself, plus
 * OEMU_ERR_INVALID_ARG when `contents` is NULL with a nonzero `contents_size`,
 * or when `contents_size` exceeds `size`.
 */
OEMU_NODISCARD oemu_status oemu_memory_map_image(oemu_memory *mem, uint64_t gva, uint64_t size,
                                                 uint32_t perms, const void *contents,
                                                 uint64_t contents_size);

/*
 * Copies `size` bytes from `src` into the guest range. Validates the whole range
 * before the first byte, so a partial copy is not observable. This is a guest
 * store and requires WRITE; to load file-backed read-only text use
 * oemu_memory_map_image instead. OEMU_ERR_INVALID_ARG for a NULL source with a
 * nonzero size.
 */
OEMU_NODISCARD oemu_status oemu_memory_write_bytes(const oemu_memory *mem, uint64_t gva,
                                                   const void *src, uint64_t size);

/*
 * Fetches the 4-byte instruction word at `pc`. Requires OEMU_PERM_EXEC and a
 * 4-byte-aligned address; a misaligned fetch is reported as OEMU_ERR_FAULT,
 * which is the architectural answer to a corrupted branch target.
 */
OEMU_NODISCARD oemu_status oemu_memory_fetch32(const oemu_memory *mem, uint64_t pc,
                                               uint32_t *word_out);

/*
 * A bus view of this memory: an oemu_memops whose callbacks call straight
 * back into the entry points above, for handing to the executor's
 * oemu_exec_step_bus() or to a syscall. The view borrows: `mem` must outlive
 * it. Constructing it allocates nothing.
 */
OEMU_NODISCARD oemu_memops oemu_memory_memops(oemu_memory *mem);

OEMU_END_DECLS

#endif /* OEMU_MEMORY_H */
