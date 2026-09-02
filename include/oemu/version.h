/* Project version information. */
#ifndef OEMU_VERSION_H
#define OEMU_VERSION_H

#include "oemu/macros.h"

OEMU_BEGIN_DECLS

#define OEMU_VERSION_MAJOR 0
#define OEMU_VERSION_MINOR 1
#define OEMU_VERSION_PATCH 0

/* Returns the version as "MAJOR.MINOR.PATCH". */
const char *oemu_version_string(void);

OEMU_END_DECLS

#endif /* OEMU_VERSION_H */
