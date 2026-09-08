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
  /* Three service spaces share the conduit, and each is answered the way the
   * oracle answers it -- never by bouncing the SMC back at the guest, which is
   * what oopsed the kernel. SMCCC splits the identifier into two bytes:
   * bits[31:24] = service class (SVC/64-bit/FAST/reserved) and bits[23:16] =
   * owner. A legacy PSCI or fast call has owner 0x00; an ARM-Standard service
   * call has owner 0x47; a hypervisor (0x45) or any other owner is not ours:
   *  - legacy PSCI: class 0x84/0xC4, owner 0x00 -- 0x84000000 + ordinal;
   *  - fast call: class 0x80/0xC0, owner 0x00 -- id 0 is FEATURES, which the
   *    kernel probes inside psci_0_2_init before it trusts any PSCI version;
   *    the oracle's dmesg ("SMC Calling Convention v1.0") fixes the answer as
   *    0x00010000, and every other fast id is NOT_SUPPORTED;
   *  - ARM-Standard service (owner 0x47, e.g. ARCH_FEATURES 0x84470002): we
   *    model no standard features, so every id there is NOT_SUPPORTED --
   *    consumed, honest, survivable.
   * Any other owner (a hypervisor's, Intel's, ...) is somebody else's service
   * and goes back to the guest's own trap path. */
  const uint64_t hi = (fnid >> 24) & 0xFFU;  /* service class byte (bits 31:24) */
  const uint64_t svc = (fnid >> 16) & 0xFFU; /* owner byte (bits 23:16) */
  const bool psci_class = (hi == 0x84U) || (hi == 0xC4U);
  const bool is_psci = psci_class && (svc == 0U);
  const bool is_fast = ((hi == 0x80U) || (hi == 0xC0U)) && (svc == 0U);
  const bool is_std = psci_class && (svc == OEMU_SMCCC_OWNER_ARM_STD);
  if (!is_psci && !is_fast && !is_std) {
    return false;
  }
  if (is_fast && ((fnid & 0xFFFFULL) == 0U)) {
    *ret0 = OEMU_SMCCC_VERSION; /* probed: v1.0 on the oracle */
    psci->calls++;
    return true;
  }
  switch (fnid) {
    case OEMU_PSCI_FN_VERSION:
      *ret0 = OEMU_PSCI_VERSION; /* probed: 0x00010001 on the oracle */
      break;
    case OEMU_PSCI_FN_FEATURES:
      /* No suspend states and no RESET2: the honest feature word is 0, and
       * the kernel then registers no cpu_suspend and sticks to SYSTEM_RESET. */
      *ret0 = 0U;
      break;
    case OEMU_PSCI_FN_SYSTEM_OFF:
      psci->halted = true;
      *ret0 = OEMU_PSCI_RET_SUCCESS;
      break;
    case OEMU_PSCI_FN_SYSTEM_RESET:
      psci->reset = true;
      *ret0 = OEMU_PSCI_RET_SUCCESS;
      break;
    default:
      /* Inside a service space we own, a call we do not model: the oracle's
       * emulator answers NOT_SUPPORTED and lets the guest live with it. */
      *ret0 = OEMU_PSCI_RET_NOT_SUPPORTED;
      break;
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
