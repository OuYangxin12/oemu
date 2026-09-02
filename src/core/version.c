#include "oemu/version.h"

/* Build the literal via the preprocessor so it cannot drift from the macros. */
#define OEMU_STRINGIFY_(x) #x
#define OEMU_STRINGIFY(x) OEMU_STRINGIFY_(x)

#define OEMU_VERSION_STRING          \
  OEMU_STRINGIFY(OEMU_VERSION_MAJOR) \
  "." OEMU_STRINGIFY(OEMU_VERSION_MINOR) "." OEMU_STRINGIFY(OEMU_VERSION_PATCH)

const char *oemu_version_string(void) { return OEMU_VERSION_STRING; }
