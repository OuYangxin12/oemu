/*
 * Tiny demo entry point: proves the library links and gives `make run`
 * something to execute. Real projects would replace this wholesale.
 */
#include <stdio.h>

#include "oemu/buffer.h"
#include "oemu/status.h"
#include "oemu/version.h"

int main(void) {
  oemu_buffer buf;
  oemu_status status = oemu_buffer_init(&buf, 0);
  if (status != OEMU_OK) {
    (void)fprintf(stderr, "init failed: %s\n", oemu_status_str(status));
    return 1;
  }

  int exit_code = 0;
  status = oemu_buffer_appendf(&buf, "oemu %s", oemu_version_string());
  if (status != OEMU_OK) {
    (void)fprintf(stderr, "append failed: %s\n", oemu_status_str(status));
    exit_code = 1;
  } else {
    const char *text = oemu_buffer_cstr(&buf);
    (void)printf("%s (len=%zu cap=%zu)\n", (text != NULL) ? text : "<oom>",
                 oemu_buffer_len(&buf), oemu_buffer_capacity(&buf));
  }

  oemu_buffer_dispose(&buf);
  return exit_code;
}
