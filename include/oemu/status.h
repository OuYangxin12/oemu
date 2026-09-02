/*
 * Library-wide status codes.
 *
 * Returning an explicit status instead of using errno keeps every failure path
 * observable from tests, which is what makes the modules unit-testable.
 */
#ifndef OEMU_STATUS_H
#define OEMU_STATUS_H

#include "oemu/macros.h"

OEMU_BEGIN_DECLS

typedef enum oemu_status {
  OEMU_OK = 0,
  OEMU_ERR_INVALID_ARG = 1, /* caller passed NULL or an out-of-range value */
  OEMU_ERR_NO_MEMORY = 2,   /* allocation failed */
  OEMU_ERR_OVERFLOW = 3,    /* size computation would wrap */
  OEMU_ERR_RANGE = 4,       /* index or length outside the valid range */
  /*
   * The instruction word is not a valid A64 encoding: an unallocated slot or a
   * reserved field combination. Distinct from UNSUPPORTED because it means the
   * guest is wrong, not that oemu is incomplete.
   */
  OEMU_ERR_DECODE = 5,
  /*
   * A valid A64 encoding that this emulator deliberately does not implement --
   * FP/SIMD, an EL1+ operation, an excluded extension. Reported rather than
   * silently executed as something else; see the README's scope section.
   */
  OEMU_ERR_UNSUPPORTED = 6
} oemu_status;

/* Returns a stable, never-NULL description for any oemu_status value. */
const char *oemu_status_str(oemu_status status);

OEMU_END_DECLS

#endif /* OEMU_STATUS_H */
