/*
 * FDT builder -- see include/oemu/fdt.h.
 *
 * Emission writes sequentially into a scratch structure block; strings
 * accumulate behind it in the same allocation. finish() then allocates
 * exactly the blob (header + strings + structure + empty reservation
 * map), copies the two blocks into their header-recorded offsets, and
 * frees the scratch. That order buys three things: name offsets are
 * final the moment a string is interned, no emission ever needs a
 * fixup or a relocation (a device tree read top-to-bottom must be
 * readable top-to-bottom, and reversed blocks are how this file's
 * earlier drafts kept producing dtc-rejecting garbage), and the blob a
 * caller hands to the guest is dense by construction.
 *
 * Every emission is all-or-nothing: fits() bounds-checks the whole
 * token before a byte moves, so a refusal leaves the builder exactly as
 * it was and open to a smaller retry (contract tests pin this down).
 */
#include "oemu/allocator.h"
#include "oemu/check.h"

#include <string.h>

#include "fdt_internal.h"

#define FDT_HEADER_SIZE  40U
#define FDT_TOKEN_SIZE   4U
#define FDT_RSV_MAP_SIZE 16U /* terminator entry (8) + pad to 8-align */

/* Scratch sizing: structure and strings share one allocation; the
 * finish-time assembly is bounded by the same cap the caller chose. */
#define FDT_MIN_CAP 128U

static size_t fdt_pad4(size_t n) {
  return (n + 3U) & ~(size_t)3U;
}

/* strnlen without POSIX: a name running past the blob is malformed
 * input to survive, not a crash to court. */
static size_t fdt_strnlen(const char *s, size_t bound) {
  size_t n = 0U;
  while ((n < bound) && (s[n] != '\0')) {
    n++;
  }
  return n;
}

static uint32_t fdt_be32(const unsigned char *p) {
  return ((uint32_t)p[0] << 24U) | ((uint32_t)p[1] << 16U) | ((uint32_t)p[2] << 8U) |
         (uint32_t)p[3];
}

static void fdt_put_be32(unsigned char *p, uint32_t v) {
  p[0] = (unsigned char)(v >> 24U);
  p[1] = (unsigned char)(v >> 16U);
  p[2] = (unsigned char)(v >> 8U);
  p[3] = (unsigned char)v;
}

/* The scratch's string block starts right after the structure region
 * reserved at init: structure never outgrows cap, so the two cursors
 * cannot meet before fits() says stop. */
static size_t fdt_strings_base(const oemu_fdt *fdt) {
  return fdt->sb_size;
}

static bool fdt_fits(const oemu_fdt *fdt, size_t need_struct, size_t need_str) {
  return fdt->sb_used + fdt->str_len + need_struct + need_str <= fdt->cap;
}

static oemu_status fdt_require_emitting(oemu_fdt *fdt) {
  OEMU_REQUIRE(fdt != NULL, "NULL oemu_fdt");
  if (fdt->sb == NULL) {
    /* finished (or never initialised): the right answer is a status, and
     * a test -- or a boot path -- must be able to ask. */
    return OEMU_ERR_STATE;
  }
  return OEMU_OK;
}

static oemu_status fdt_emit_raw(oemu_fdt *fdt, const unsigned char *bytes, size_t len) {
  (void)memcpy(fdt->sb + fdt->sb_used, bytes, len);
  fdt->sb_used += len;
  return OEMU_OK;
}

static oemu_status fdt_emit_token(oemu_fdt *fdt, uint32_t token) {
  unsigned char word[FDT_TOKEN_SIZE];
  fdt_put_be32(word, token);
  return fdt_emit_raw(fdt, word, sizeof(word));
}

/* Intern a property name; the returned string-block offset is final --
 * strings append forward and never move. */
static uint32_t fdt_intern(oemu_fdt *fdt, const char *s) {
  const size_t len = strlen(s) + 1U;
  (void)memcpy(fdt->sb + fdt_strings_base(fdt) + fdt->str_len, s, len);
  const uint32_t off = (uint32_t)fdt->str_len;
  fdt->str_len += len;
  return off;
}

oemu_status oemu_fdt_init(oemu_fdt *fdt, size_t cap) {
  if ((fdt == NULL) || (cap < FDT_MIN_CAP)) {
    return OEMU_ERR_INVALID_ARG;
  }
  const oemu_allocator *alloc = oemu_allocator_get();
  /* One scratch for both blocks; the structure region reserves the full
   * cap (worst case: a tree with no strings), and strings share the tail
   * of the same allocation only because fits() keeps their sum under cap.
   * Structure bytes live at [0, sb_used); strings at [sb_size, ...). */
  unsigned char *sb = (unsigned char *)alloc->alloc(cap + cap, alloc->user_data);
  if (sb == NULL) {
    return OEMU_ERR_NO_MEMORY;
  }
  (void)memset(fdt, 0, sizeof(*fdt));
  fdt->sb = sb;
  fdt->sb_size = cap;
  fdt->cap = cap;
  const oemu_status st = oemu_fdt_begin_node(fdt, "");
  if (st != OEMU_OK) {
    alloc->free(sb, alloc->user_data);
    (void)memset(fdt, 0, sizeof(*fdt));
    return st;
  }
  return OEMU_OK;
}

void oemu_fdt_dispose(oemu_fdt *fdt) {
  if (fdt == NULL) {
    return;
  }
  const oemu_allocator *alloc = oemu_allocator_get();
  if (fdt->sb != NULL) {
    alloc->free(fdt->sb, alloc->user_data);
  } else if (fdt->data != NULL) {
    alloc->free(fdt->data, alloc->user_data);
  }
  (void)memset(fdt, 0, sizeof(*fdt));
}

oemu_status oemu_fdt_begin_node(oemu_fdt *fdt, const char *name) {
  oemu_status st = fdt_require_emitting(fdt);
  if (st != OEMU_OK) {
    return st;
  }
  if (name == NULL) {
    return OEMU_ERR_INVALID_ARG;
  }
  const size_t name_len = strlen(name);
  if (name_len >= OEMU_FDT_MAX_NAME) {
    return OEMU_ERR_INVALID_ARG;
  }
  const size_t inline_bytes = fdt_pad4(name_len + 1U); /* "" pads to one word */
  if (!fdt_fits(fdt, FDT_TOKEN_SIZE + inline_bytes, 0U)) {
    return OEMU_ERR_NO_MEMORY;
  }
  st = fdt_emit_token(fdt, OEMU_FDT_TOKEN_BEGIN_NODE);
  if (st != OEMU_OK) {
    return st;
  }
  unsigned char scratch[OEMU_FDT_MAX_NAME];
  (void)memcpy(scratch, name, name_len);
  (void)memset(scratch + name_len, 0, inline_bytes - name_len);
  st = fdt_emit_raw(fdt, scratch, inline_bytes);
  if (st == OEMU_OK) {
    fdt->depth += 1;
  }
  return st;
}

oemu_status oemu_fdt_end_node(oemu_fdt *fdt) {
  oemu_status st = fdt_require_emitting(fdt);
  if (st != OEMU_OK) {
    return st;
  }
  if (fdt->depth <= 0) {
    return OEMU_ERR_STATE;
  }
  if (!fdt_fits(fdt, FDT_TOKEN_SIZE, 0U)) {
    return OEMU_ERR_NO_MEMORY;
  }
  st = fdt_emit_token(fdt, OEMU_FDT_TOKEN_END_NODE);
  if (st == OEMU_OK) {
    fdt->depth -= 1;
  }
  return st;
}

/* Property prologue: shared checks plus the whole token's fits() test,
 * before the name is interned -- a refusal must not leave a stray
 * string behind either. */
static oemu_status fdt_prop_guard(oemu_fdt *fdt, const char *name, size_t value_len,
                                  uint32_t *name_off) {
  oemu_status st = fdt_require_emitting(fdt);
  if (st != OEMU_OK) {
    return st;
  }
  if ((name == NULL) || (name[0] == '\0')) {
    return OEMU_ERR_INVALID_ARG;
  }
  if (fdt->depth <= 0) {
    return OEMU_ERR_STATE; /* properties need an open node */
  }
  const size_t name_bytes = strlen(name) + 1U;
  if ((name_bytes > OEMU_FDT_MAX_NAME) ||
      !fdt_fits(fdt, FDT_TOKEN_SIZE + 8U + fdt_pad4(value_len), name_bytes)) {
    return OEMU_ERR_NO_MEMORY;
  }
  *name_off = fdt_intern(fdt, name);
  return OEMU_OK;
}

static oemu_status fdt_prop_emit(oemu_fdt *fdt, uint32_t name_off, const unsigned char *val,
                                 size_t val_len) {
  unsigned char head[12];
  fdt_put_be32(head, OEMU_FDT_TOKEN_PROP);
  /* The PROP body is [length][name offset]: the spec order, dtc's order.
   * Reading these two the wrong way round is a classic trap -- a builder
   * and reader that swap them agree with each other and with nobody else. */
  fdt_put_be32(head + 4U, (uint32_t)val_len);
  fdt_put_be32(head + 8U, name_off);
  oemu_status s = fdt_emit_raw(fdt, head, sizeof(head));
  if ((s == OEMU_OK) && (val_len != 0U)) {
    s = fdt_emit_raw(fdt, val, val_len);
  }
  const size_t pad = fdt_pad4(val_len) - val_len;
  if ((s == OEMU_OK) && (pad != 0U)) {
    const unsigned char zeros[4] = {0U, 0U, 0U, 0U};
    s = fdt_emit_raw(fdt, zeros, pad);
  }
  return s;
}

oemu_status oemu_fdt_prop_empty(oemu_fdt *fdt, const char *name) {
  uint32_t off = 0U;
  const oemu_status st = fdt_prop_guard(fdt, name, 0U, &off);
  if (st != OEMU_OK) {
    return st;
  }
  return fdt_prop_emit(fdt, off, NULL, 0U);
}

oemu_status oemu_fdt_prop_u32(oemu_fdt *fdt, const char *name, uint32_t value) {
  uint32_t off = 0U;
  const oemu_status st = fdt_prop_guard(fdt, name, 4U, &off);
  if (st != OEMU_OK) {
    return st;
  }
  unsigned char word[4];
  fdt_put_be32(word, value);
  return fdt_prop_emit(fdt, off, word, sizeof(word));
}

oemu_status oemu_fdt_prop_u64(oemu_fdt *fdt, const char *name, uint64_t value) {
  uint32_t off = 0U;
  const oemu_status st = fdt_prop_guard(fdt, name, 8U, &off);
  if (st != OEMU_OK) {
    return st;
  }
  unsigned char word[8];
  fdt_put_be32(word, (uint32_t)(value >> 32U));
  fdt_put_be32(word + 4U, (uint32_t)value);
  return fdt_prop_emit(fdt, off, word, sizeof(word));
}

oemu_status oemu_fdt_prop_str(oemu_fdt *fdt, const char *name, const char *value) {
  if (value == NULL) {
    (void)fdt_require_emitting(fdt);
    return OEMU_ERR_INVALID_ARG;
  }
  uint32_t off = 0U;
  const size_t len = strlen(value) + 1U;
  const oemu_status st = fdt_prop_guard(fdt, name, len, &off);
  if (st != OEMU_OK) {
    return st;
  }
  return fdt_prop_emit(fdt, off, (const unsigned char *)value, len);
}

oemu_status oemu_fdt_prop_cells(oemu_fdt *fdt, const char *name, const uint32_t *cells,
                                size_t count) {
  if (cells == NULL) {
    (void)fdt_require_emitting(fdt);
    return OEMU_ERR_INVALID_ARG;
  }
  uint32_t off = 0U;
  const size_t len = count * 4U; /* cell counts stay sane; see card D-M4a-3 */
  const oemu_status st = fdt_prop_guard(fdt, name, len, &off);
  if (st != OEMU_OK) {
    return st;
  }
  /* Big-endian u32s, emitted word by word -- no scratch to overflow. */
  unsigned char head[12];
  fdt_put_be32(head, OEMU_FDT_TOKEN_PROP);
  fdt_put_be32(head + 4U, (uint32_t)len); /* length before name, per dtc */
  fdt_put_be32(head + 8U, off);
  oemu_status s = fdt_emit_raw(fdt, head, sizeof(head));
  for (size_t i = 0U; (s == OEMU_OK) && (i < count); ++i) {
    unsigned char word[4];
    fdt_put_be32(word, cells[i]);
    s = fdt_emit_raw(fdt, word, sizeof(word));
  }
  return s;
}

oemu_status oemu_fdt_finish(oemu_fdt *fdt) {
  oemu_status st = fdt_require_emitting(fdt);
  if (st != OEMU_OK) {
    return st;
  }
  if (fdt->depth != 0) {
    return OEMU_ERR_STATE; /* refuse to close a tree with nodes open */
  }
  /* cap counts emitted scratch bytes; the header and rsvmap are assembled
   * around them and cost no scratch. So all finish() itself still needs
   * is one word for the closing ENDTREE. */
  if (!fdt_fits(fdt, FDT_TOKEN_SIZE, 0U)) {
    return OEMU_ERR_NO_MEMORY;
  }
  /* Close the structure block with a bare ENDTREE -- no padding, exactly
   * as the oracle blob ends it. */
  st = fdt_emit_token(fdt, OEMU_FDT_TOKEN_END);
  if (st != OEMU_OK) {
    return st;
  }
  /* Version 17 grew two fields -- the size of each block -- and libfdt
   * bounds-checks with them. Omit them (dtc calls a v17 blob with zero
   * sizes truncated) and every tool rejects the tree. */
  const size_t struct_off = FDT_HEADER_SIZE + FDT_RSV_MAP_SIZE;
  const size_t strings_off = struct_off + fdt->sb_used;
  const size_t total = strings_off + fdt->str_len;
  const oemu_allocator *alloc = oemu_allocator_get();
  unsigned char *blob = (unsigned char *)alloc->alloc(total, alloc->user_data);
  if (blob == NULL) {
    return OEMU_ERR_NO_MEMORY; /* refusal keeps the tree open and retryable */
  }
  (void)memset(blob, 0, struct_off);
  fdt_put_be32(blob + 0x00U, OEMU_FDT_MAGIC);
  fdt_put_be32(blob + 0x04U, (uint32_t)total);
  fdt_put_be32(blob + 0x08U, (uint32_t)struct_off);
  fdt_put_be32(blob + 0x0CU, (uint32_t)strings_off);
  fdt_put_be32(blob + 0x10U, (uint32_t)FDT_HEADER_SIZE);
  fdt_put_be32(blob + 0x14U, OEMU_FDT_V17);
  fdt_put_be32(blob + 0x18U, 16U);                    /* last compatible version: v16 */
  fdt_put_be32(blob + 0x20U, (uint32_t)fdt->str_len); /* v17: the two block */
  fdt_put_be32(blob + 0x24U, (uint32_t)fdt->sb_used); /* sizes, mandatory   */
  (void)memcpy(blob + struct_off, fdt->sb, fdt->sb_used);
  (void)memcpy(blob + strings_off, fdt->sb + fdt_strings_base(fdt), fdt->str_len);
  alloc->free(fdt->sb, alloc->user_data);
  fdt->sb = NULL;
  fdt->data = blob;
  fdt->len = total;
  fdt->finished = true;
  return OEMU_OK;
}

const unsigned char *oemu_fdt_bytes(const oemu_fdt *fdt) {
  OEMU_REQUIRE(fdt != NULL, "NULL oemu_fdt");
  return fdt->data;
}

size_t oemu_fdt_length(const oemu_fdt *fdt) {
  OEMU_REQUIRE(fdt != NULL, "NULL oemu_fdt");
  return fdt->len;
}

/* --- the read-back helper (internal) -------------------------------------- */

oemu_status oemu_fdt_internal_total_size(const unsigned char *blob, size_t blob_len,
                                         size_t *out) {
  if ((blob == NULL) || (out == NULL)) {
    return OEMU_ERR_INVALID_ARG;
  }
  if (blob_len < FDT_HEADER_SIZE) {
    return OEMU_ERR_INVALID_ARG;
  }
  if (fdt_be32(blob) != OEMU_FDT_MAGIC) {
    return OEMU_ERR_FORMAT;
  }
  const uint32_t total = fdt_be32(blob + 4U);
  if ((size_t)total > blob_len) {
    return OEMU_ERR_FORMAT;
  }
  *out = total;
  return OEMU_OK;
}

oemu_status oemu_fdt_internal_find(const unsigned char *blob, size_t blob_len, const char *path,
                                   const char *prop, const unsigned char **out, size_t *len) {
  if ((blob == NULL) || (path == NULL) || (prop == NULL) || (out == NULL) || (len == NULL)) {
    return OEMU_ERR_INVALID_ARG;
  }
  *out = NULL;
  *len = 0U;
  if (blob_len < FDT_HEADER_SIZE) {
    return OEMU_ERR_INVALID_ARG;
  }
  if (fdt_be32(blob) != OEMU_FDT_MAGIC) {
    return OEMU_ERR_FORMAT;
  }
  const uint32_t off_struct = fdt_be32(blob + 8U);
  const uint32_t off_strings = fdt_be32(blob + 12U);
  if ((off_struct >= blob_len) || (off_strings > blob_len)) {
    return OEMU_ERR_FORMAT;
  }
  const unsigned char *p = blob + off_struct;
  const unsigned char *const end = blob + blob_len;
  /* Path matching is a stack: one frame per open depth recording how far
   * into `path` the ancestors (with this node) matched, and whether this
   * node itself is the one we came for. */
  typedef struct {
    size_t pos;
    bool match;
    bool target;
  } fdt_frame;
  fdt_frame stack[32];
  int depth = -1;
  const size_t path_len = strlen(path);
  while (p + 4U <= end) {
    const uint32_t token = fdt_be32(p);
    p += 4U;
    if (token == OEMU_FDT_TOKEN_END) {
      break;
    }
    if (token == OEMU_FDT_TOKEN_END_NODE) {
      if (depth >= 0) {
        if (stack[depth].target) {
          return OEMU_ERR_NOT_FOUND; /* left the target without finding it */
        }
        depth--;
      }
      continue;
    }
    if (token == OEMU_FDT_TOKEN_BEGIN_NODE) {
      const char *nm = (const char *)p;
      const size_t nl = fdt_strnlen(nm, (size_t)(end - p));
      const size_t adv = fdt_pad4(nl + 1U);
      if (p + adv > end) {
        return OEMU_ERR_FORMAT;
      }
      p += adv;
      depth++;
      if (depth >= (int)(sizeof(stack) / sizeof(stack[0]))) {
        return OEMU_ERR_FORMAT; /* absurdly nested tree */
      }
      stack[depth].pos = 0U;
      stack[depth].match = false;
      stack[depth].target = false;
      const bool parent_target = (depth > 0) && stack[depth - 1].target;
      if (parent_target) {
        continue; /* nothing lives inside the node we were handed */
      }
      if ((depth > 0) && !stack[depth - 1].match) {
        continue; /* a mismatched ancestor disqualifies the subtree */
      }
      if (depth == 0) {
        if (nl != 0U) {
          continue; /* the first node must be the unnamed root */
        }
        stack[depth].pos = (path_len == 1U) ? path_len : 0U;
        stack[depth].match = true;
        stack[depth].target = (path_len == 1U);
        continue;
      }
      /* Compare this node's name with the next path component. */
      const size_t start = stack[depth - 1].pos + 1U;
      if (start > path_len) {
        continue;
      }
      size_t wl = 0U;
      while ((start + wl < path_len) && (path[start + wl] != '/')) {
        wl++;
      }
      if ((nl == wl) && ((wl == 0U) || (memcmp(nm, path + start, wl) == 0))) {
        stack[depth].pos = start + wl;
        stack[depth].match = true;
        stack[depth].target = (start + wl == path_len);
      }
      continue;
    }
    if (token == OEMU_FDT_TOKEN_PROP) {
      /* The PROP body: length word at token+4, name offset at token+8, the
       * value starting at token+12. (dtc's order -- a builder and reader
       * that agree on the swapped order match nothing else on earth.) */
      if (p + 12U > end) {
        return OEMU_ERR_FORMAT;
      }
      const uint32_t dlen = fdt_be32(p);
      const uint32_t name_off = fdt_be32(p + 4U);
      p += 8U;
      if ((p + dlen > end) || (off_strings + name_off >= blob_len)) {
        return OEMU_ERR_FORMAT;
      }
      const char *nm = (const char *)blob + off_strings + name_off;
      const unsigned char *value = p;
      p += fdt_pad4(dlen);
      if ((depth >= 0) && stack[depth].target && (strcmp(nm, prop) == 0)) {
        *out = value;
        *len = dlen;
        return OEMU_OK;
      }
      continue;
    }
    return OEMU_ERR_FORMAT; /* unknown token */
  }
  return OEMU_ERR_NOT_FOUND;
}
