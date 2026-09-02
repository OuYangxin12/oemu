#include "oemu/check.h"

#include <stdio.h>
#include <stdlib.h>

/*
 * Flush gcov counters before aborting.
 *
 * abort() terminates the process abnormally, so the coverage runtime never gets
 * to write its counters -- this function would report 0% despite being covered
 * by every death test. __gcov_dump writes the accumulated data out first.
 *
 * OEMU_COVERAGE is defined by cmake/Coverage.cmake; GCC defines no macro of its
 * own for --coverage, so the build system has to say so explicitly. The symbol
 * is declared rather than included from gcov.h, which is not universally
 * available.
 */
#if defined(OEMU_COVERAGE) && defined(__GNUC__) && !defined(__clang__)
extern void __gcov_dump(void);
#define OEMU_DUMP_COVERAGE() __gcov_dump()
#else
#define OEMU_DUMP_COVERAGE() ((void)0)
#endif

void oemu_check_fail(const char *file, int line, const char *expr_text, const char *message) {
  (void)fprintf(stderr, "%s:%d: check failed: %s: %s\n", (file != NULL) ? file : "<unknown>",
                line, (expr_text != NULL) ? expr_text : "<expr>",
                (message != NULL) ? message : "");
  (void)fflush(stderr);
  OEMU_DUMP_COVERAGE();
  abort();
}
