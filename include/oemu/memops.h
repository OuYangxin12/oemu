/*
 * The bus seam between the executor and whatever backs guest memory.
 *
 * oemu_memops names the four operations the executor needs -- fetch, read,
 * write, validate -- as function pointers over an opaque context. Two
 * implementations exist today, oemu_memory_memops() and
 * oemu_aspace_memops(), and both are plain adapters: a view adds nothing an
 * owner does not already do, and its callbacks tolerate a NULL context
 * exactly the way the module's own entry points do.
 *
 * Every provider shares one contract, so the executor never has to know
 * which one it is talking to:
 *   - addresses are physical (or, for oemu_memory, the flat guest space);
 *   - `validate` sizes are BYTE counts and it reads or writes nothing;
 *   - `fetch32` requires OEMU_PERM_EXEC and four-byte alignment;
 *   - an out-of-range, cross-region or under-permissioned access comes back
 *     OEMU_ERR_FAULT, never a crash.
 *
 * oemu_env_ops is the matching seam for the guest environment: what turns an
 * SVC into a syscall, and how the run loop learns the guest has stopped. The
 * syscall callback takes a memops view because a syscall reads and writes
 * guest memory through the same bus the instructions do.
 *
 * The structs are plain values: build one on the stack per call site, or
 * keep it in a machine. Constructing a view allocates nothing.
 *
 * Deliberately, this header does NOT include oemu/memory.h: memory.h and
 * aspace.h include this one to declare their constructor, and pulling the
 * other way would close the cycle. The OEMU_PERM_* bits a caller supplies
 * to `validate` come along with whichever owner header it already has.
 */
#ifndef OEMU_MEMOPS_H
#define OEMU_MEMOPS_H

#include "oemu/decode.h"
#include "oemu/macros.h"
#include "oemu/status.h"

#include <stdbool.h>
#include <stdint.h>

OEMU_BEGIN_DECLS

typedef struct oemu_memops {
  void *ctx; /* the owner; each callback casts it back */
  /* Fetches the 4-byte instruction word at `pa`: EXEC permission, 4-byte
   * alignment. */
  OEMU_NODISCARD oemu_status (*fetch32)(void *ctx, uint64_t pa, uint32_t *word_out);
  /* Reads `size` little-endian bytes, optionally sign-extending to 64 bits. */
  OEMU_NODISCARD oemu_status (*read)(void *ctx, uint64_t pa, oemu_mem_size size,
                                     bool sign_extend, uint64_t *value_out);
  /* Writes the low (1 << size) little-endian bytes of `value`. */
  OEMU_NODISCARD oemu_status (*write)(void *ctx, uint64_t pa, oemu_mem_size size,
                                      uint64_t value);
  /* Reports whether [pa, pa+size) -- `size` in BYTES -- lies wholly inside
   * one region carrying at least `perms`, without touching memory. */
  OEMU_NODISCARD oemu_status (*validate)(void *ctx, uint64_t pa, uint64_t size, uint32_t perms);
} oemu_memops;

typedef struct oemu_env_ops {
  void *ctx; /* the environment; each callback casts it back */
  /* Executes one syscall: `nr` and the x0..x5 arguments. Returns the value
   * the guest receives in x0 (a negative errno on failure). Guest memory is
   * reached through `mem`. */
  int64_t (*syscall)(void *ctx, const oemu_memops *mem, uint64_t nr, const uint64_t args[6]);
  /* True once the guest has stopped (exit, powerdown, ...). */
  bool (*halted)(const void *ctx);
} oemu_env_ops;

OEMU_END_DECLS

#endif /* OEMU_MEMOPS_H */
