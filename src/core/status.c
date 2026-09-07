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
    case OEMU_ERR_FAULT:
      return "guest memory or trap fault";
    case OEMU_ERR_TIMEOUT:
      return "instruction budget exhausted";
    case OEMU_ERR_BLOCKED:
      return "vcpu parked on wait-for-interrupt";
    case OEMU_ERR_STATE:
      return "call outside the object's lifecycle";
    case OEMU_ERR_FORMAT:
      return "data carries the format's shape but breaks its rules";
    case OEMU_ERR_NOT_FOUND:
      return "looked up, not present";
    case OEMU_ERR_FULL:
      return "bounded resource full";
    default:
      break;
  }
  return "unknown status";
}
