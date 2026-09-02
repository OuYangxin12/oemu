/*
 * Fatal-precondition checks.
 *
 * The buffer API reports misuse with a status code, so it needs no death tests.
 * Some contracts, though, are better enforced by aborting: this module shows the
 * pattern and gives the harness a death test to exercise.
 *
 * OEMU_REQUIRE stays active in release builds on purpose (unlike assert), so the
 * contract cannot silently disappear from a shipped binary.
 */
#ifndef OEMU_CHECK_H
#define OEMU_CHECK_H

#include "oemu/macros.h"

OEMU_BEGIN_DECLS

/*
 * Prints "<file>:<line>: <expr_text>: <message>" to stderr and aborts.
 * Never returns.
 */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noreturn))
#endif
void oemu_check_fail(const char *file, int line, const char *expr_text, const char *message);

/*
 * Aborts with a diagnostic when `expr` is false. Use for programming errors that
 * cannot be reported to the caller; use status codes for anything recoverable.
 */
#define OEMU_REQUIRE(expr, message)                                     \
  do {                                                                  \
    if (!(expr)) {                                                      \
      oemu_check_fail(__FILE__, __LINE__, #expr, (message));            \
    }                                                                   \
  } while (0)

OEMU_END_DECLS

#endif /* OEMU_CHECK_H */
