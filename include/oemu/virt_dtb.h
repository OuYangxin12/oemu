/*
 * The `oemu virt` device tree: a small, virt-shaped FDT generated at boot time
 * rather than shipped as an opaque blob, so the emulator controls exactly the
 * nodes it can honour and can point the guest at an initrd without needing an
 * offline `dtc`. The shape mirrors what qemu-system-aarch64 -machine virt hands
 * a guest (and docs/linux-minimal-qemu.md validated the boot against): one
 * A53, a GICv2, an arch timer, a fixed-clock PL011, a PSCI node with the SMC
 * conduit, and a /chosen the kernel reads for its command line and initrd.
 *
 * The tree is emitted with the append-only builder (oemu/fdt): the caller owns
 * a sized oemu_fdt, virt_dtb fills it, and a full buffer surfaces as a
 * recoverable OEMU_ERR_NO_MEMORY -- emission never allocates mid-tree.
 *
 * /chosen always carries linux,initrd-start and linux,initrd-end (each an
 * 8-byte cell pair under the root's #address-cells = 2), set to 0 when no
 * initrd is loaded: the kernel only consumes the initrd when start is nonzero,
 * so emitting zeros is honest and keeps the property layout fixed.
 */
#ifndef OEMU_VIRT_DTB_H
#define OEMU_VIRT_DTB_H

#include "oemu/fdt.h"
#include "oemu/macros.h"
#include "oemu/status.h"

#include <stdint.h>

OEMU_BEGIN_DECLS

/* Everything the emitted tree varies on; the fixed layout constants (GIC at
 * 0x08000000, UART at 0x09000000, ...) are baked to match the machine the boot
 * command assembles, so they are not knobs here. */
typedef struct oemu_virt_dtb_params {
  uint64_t ram_base;     /* where RAM starts (the /memory reg address cell) */
  uint64_t ram_size;     /* bytes of RAM the machine actually builds */
  uint64_t initrd_start; /* 0 when no initrd; else the load physical address */
  uint64_t initrd_end;   /* one past the last initrd byte (0 when none) */
  const char *cmdline;   /* /chosen/bootargs (NULL -> a safe default) */
} oemu_virt_dtb_params;

/* Emit the whole virt tree into `fdt` and finish it (the caller supplies the
 * capacity and disposes it). After a successful call bytes()/length() describe
 * the blob to hand the guest in x0. */
OEMU_NODISCARD oemu_status oemu_virt_dtb_build(oemu_fdt *fdt,
                                               const oemu_virt_dtb_params *params);

OEMU_END_DECLS

#endif /* OEMU_VIRT_DTB_H */
