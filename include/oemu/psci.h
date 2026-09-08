/*
 * PSCI -- the firmware interface arm64 Linux talks power through.
 *
 * PSCI is not an instruction: SMC and HVC are, and this module answers
 * the calls that travel on them. oemu has no EL3, so the conduit hook
 * (oemu_env_ops' fw_call) delivers directly, the same contract QEMU's
 * secure firmware fulfils for it. The DTB the boot path generates says
 * method="smc" because SMC is the conduit oemu actually intercepts.
 *
 * Function IDs follow SMCCC's legacy PSCI service: the SMC32 call carries
 * 0x84000000 and the SMC64 form of the same call sets the width bit,
 * 0xC4000000, so both widths travel. The values cross-check against the
 * guest's own contract: include/uapi/linux/psci.h in the very kernel Image
 * we boot. An earlier draft guessed SYSTEM_OFF/RESET as 0x84000002 and
 * 0x84000003 (they are CPU_OFF and CPU_ON), so the kernel's panic-reboot
 * SMC fell through to the exception path and oopsed instead of powering
 * off. VERSION answers 0x00010001 (PSCI v1.1, probed off
 * qemu-system-aarch64 10.2.1); an unknown call inside the PSCI service
 * answers NOT_SUPPORTED exactly like the oracle's emulator does. M4b
 * extends CPU_ON/CPU_OFF (SMP) once the interrupt controller exists.
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
/* SMCCC standard-service answers. The kernel negotiates SMCCC before it
 * trusts PSCI -- drivers/firmware/smccc probes FEATURES (a FAST call at
 * 0x80000000) inside psci_0_2_init itself, so a machine that claims
 * arm,psci-1.0 in its device tree must answer it. Measured off the oracle's
 * own dmesg: "psci: SMC Calling Convention v1.0", so FEATURES answers
 * 0x00010000 and no architecture features (TRNG, SoC-id) are claimed. */
#define OEMU_SMCCC_FEATURES      0x80000000ULL /* SMC32 fast, id 0 */
#define OEMU_SMCCC_FEATURES_64   0xC0000000ULL /* SMC64 fast, id 0 */
#define OEMU_SMCCC_VERSION       0x00010000ULL /* probed: v1.0 on the oracle */
#define OEMU_SMCCC_OWNER_ARM_STD 0x47ULL
/* The service bases. A call is PSCI's iff it lies in one of the two 256-wide
 * windows; ordinals count from the base in either width. */
#define OEMU_PSCI_FN_BASE   0x84000000ULL /* SMC32 */
#define OEMU_PSCI_FN64_BASE 0xC4000000ULL /* SMC64 (the 0x40000000 bit set) */
#define OEMU_PSCI_FN_WINDOW 0x100ULL
/* The calls we actually answer (both widths where SMCCC defines both). */
#define OEMU_PSCI_FN_VERSION          OEMU_PSCI_FN_BASE          /* FN(0)  */
#define OEMU_PSCI_FN_CPU_SUSPEND      (OEMU_PSCI_FN_BASE + 1U)   /* FN(1)  */
#define OEMU_PSCI_FN_CPU_SUSPEND_64   (OEMU_PSCI_FN64_BASE + 1U) /* FN64(1)*/
#define OEMU_PSCI_FN_CPU_OFF          (OEMU_PSCI_FN_BASE + 2U)   /* FN(2)  */
#define OEMU_PSCI_FN_CPU_ON           (OEMU_PSCI_FN_BASE + 3U)   /* FN(3)  */
#define OEMU_PSCI_FN_CPU_ON_64        (OEMU_PSCI_FN64_BASE + 3U) /* FN64(3)*/
#define OEMU_PSCI_FN_AFFINITY_INFO    (OEMU_PSCI_FN_BASE + 4U)   /* FN(4)  */
#define OEMU_PSCI_FN_AFFINITY_INFO_64 (OEMU_PSCI_FN64_BASE + 4U) /* FN64(4)*/
#define OEMU_PSCI_FN_FEATURES         (OEMU_PSCI_FN_BASE + 10U)  /* FN(10) */
#define OEMU_PSCI_FN_SYSTEM_RESET2    (OEMU_PSCI_FN_BASE + 18U)  /* FN(18) */
#define OEMU_PSCI_FN_SYSTEM_RESET2_64 (OEMU_PSCI_FN64_BASE + 18U)
#define OEMU_PSCI_FN_SYSTEM_OFF       (OEMU_PSCI_FN_BASE + 8U) /* FN(8)  */
#define OEMU_PSCI_FN_SYSTEM_RESET     (OEMU_PSCI_FN_BASE + 9U) /* FN(9)  */
#define OEMU_PSCI_RET_SUCCESS         0ULL
#define OEMU_PSCI_RET_NOT_SUPPORTED   (~0ULL)

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
