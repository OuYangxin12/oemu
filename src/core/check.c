#include "oemu/check.h"

#include <stdio.h>
#include <stdlib.h>

void oemu_check_fail(const char *file, int line, const char *expr_text,
                     const char *message) {
  (void)fprintf(stderr, "%s:%d: check failed: %s: %s\n",
                (file != NULL) ? file : "<unknown>", line,
                (expr_text != NULL) ? expr_text : "<expr>",
                (message != NULL) ? message : "");
  (void)fflush(stderr);
  abort();
}
