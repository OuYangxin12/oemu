/*
 * PSCI -- the firmware interface arm64 Linux talks power through.
 *
 * PSCI is not an instruction: SMC and HVC are, and this module answers
 * the calls that travel on them. oemu has no EL3, so the conduit hook
 * (oemu_env_ops' fw_call) delivers directly, the same contract QEMU's
 * secure firmware fulfils for it. The DTB the boot path generates says
 * method="smc" because SMC is the conduit oemu actually intercepts.
 *
 * Function IDs and return values follow the measured oracle: VERSION
 * answers 0x00010001 (PSCI v1.1, probed off qemu-system-aarch64 10.2.1),
 * SYSTEM_OFF halts the machine with success, unknown calls answer
 * NOT_SUPPORTED (-1) exactly like the oracle does for calls outside its
 * model. M4b extends this skeleton with CPU_ON/CPU_OFF (SMP) once the
 * interrupt controller exists.
 */
#ifndef OEMU_PSCI_H
#define OEMU_PSCI_H

#include "oemu/macros.h"
#include "oemu/memops.h"

#include <stdbool.h>
#include <stdint.h>

OEMU_BEGIN_DECLS

/* The version a guest learns by calling PSCI_VERSION. */
#define OEMU_PSCI_VERSION 0x00010001ULL
/* Function IDs (SMC32 encoding, 64-bit calls share the low half today). */
#define OEMU_PSCI_FN_CPU_SUSPEND    0x84000001ULL
#define OEMU_PSCI_FN_SYSTEM_OFF     0x84000002ULL
#define OEMU_PSCI_FN_SYSTEM_RESET   0x84000003ULL
#define OEMU_PSCI_FN_VERSION        0x84000000ULL
#define OEMU_PSCI_RET_SUCCESS       0ULL
#define OEMU_PSCI_RET_NOT_SUPPORTED (~0ULL)

typedef struct oemu_psci {
  bool halted;    /* SYSTEM_OFF landed */
  bool reset;     /* SYSTEM_RESET landed (the boot loop decides) */
  uint64_t calls; /* calls consumed, for observability */
} oemu_psci;

void oemu_psci_init(oemu_psci *psci);

/* The environment view the executor wants: fw_call answers PSCI, halted
 * reports the power state. Mirrors oemu_sysenv_envops by shape. */
oemu_env_ops oemu_psci_envops(oemu_psci *psci);

/* The conduit body, callable directly (tests, and the env callback):
 * consumes a call, fills *ret0, returns whether it was PSCI's. SMC and
 * HVC differ only in where they came from. */
bool oemu_psci_dispatch(oemu_psci *psci, uint64_t fnid, uint64_t *ret0);

OEMU_END_DECLS

#endif /* OEMU_PSCI_H */
