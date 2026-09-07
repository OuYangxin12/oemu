/*
 * PSCI conduit implementation -- see include/oemu/psci.h.
 */
#include "oemu/psci.h"

#include "oemu/check.h"

#include <stddef.h>

void oemu_psci_init(oemu_psci *psci) {
  OEMU_REQUIRE(psci != NULL, "NULL oemu_psci");
  psci->halted = false;
  psci->reset = false;
  psci->calls = 0U;
}

bool oemu_psci_dispatch(oemu_psci *psci, uint64_t fnid, uint64_t *ret0) {
  if ((psci == NULL) || (ret0 == NULL)) {
    return false;
  }
  switch (fnid) {
    case OEMU_PSCI_FN_VERSION:
      *ret0 = OEMU_PSCI_VERSION; /* probed: 0x00010001 on the oracle */
      break;
    case OEMU_PSCI_FN_SYSTEM_OFF:
      psci->halted = true;
      *ret0 = OEMU_PSCI_RET_SUCCESS;
      break;
    case OEMU_PSCI_FN_SYSTEM_RESET:
      psci->reset = true;
      *ret0 = OEMU_PSCI_RET_SUCCESS;
      break;
    case OEMU_PSCI_FN_CPU_SUSPEND:
      *ret0 = OEMU_PSCI_RET_NOT_SUPPORTED;
      break;
    default:
      return false; /* not a PSCI call: the conduit goes back to the guest */
  }
  psci->calls++;
  return true;
}

/* --- the env view --------------------------------------------------------- */

static bool psci_fw_call(void *ctx, bool is_hvc, uint16_t imm, const uint64_t args[3],
                         uint64_t *ret0) {
  (void)is_hvc;
  /* The conduit carries the function id in x0; imm selects the call
   * family and is not consulted (PSCI32/64 both travel #0). */
  (void)imm;
  return oemu_psci_dispatch((oemu_psci *)ctx, args[0], ret0);
}

static bool psci_halted(const void *ctx) {
  const oemu_psci *psci = (const oemu_psci *)ctx;
  return psci->halted || psci->reset;
}

oemu_env_ops oemu_psci_envops(oemu_psci *psci) {
  OEMU_REQUIRE(psci != NULL, "NULL oemu_psci");
  oemu_env_ops ops;
  ops.ctx = psci;
  ops.syscall = NULL; /* a booted kernel calls the kernel, not the host */
  ops.halted = &psci_halted;
  ops.fw_call = &psci_fw_call;
  return ops;
}
