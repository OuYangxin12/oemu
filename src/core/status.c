#include "oemu/status.h"

const char *oemu_status_str(oemu_status status) {
  switch (status) {
    case OEMU_OK:
      return "ok";
    case OEMU_ERR_INVALID_ARG:
      return "invalid argument";
    case OEMU_ERR_NO_MEMORY:
      return "out of memory";
    case OEMU_ERR_OVERFLOW:
      return "size overflow";
    case OEMU_ERR_RANGE:
      return "out of range";
    case OEMU_ERR_DECODE:
      return "undefined instruction encoding";
    case OEMU_ERR_UNSUPPORTED:
      return "instruction outside the emulated subset";
    default:
      break;
  }
  return "unknown status";
}
