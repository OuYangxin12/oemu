/*
 * The `oemu virt` device tree builder -- see include/oemu/virt_dtb.h.
 *
 * Reproduces the node set qemu-system-aarch64 -machine virt exposes (and that
 * docs/linux-minimal-qemu.md booted against): /chosen (bootargs + initrd cells
 * + stdout-path), /aliases, a PSCI node on the SMC conduit, one A53 under
 * /cpus, a GICv2 interrupt controller, an armv8 arch timer, a fixed-clock
 * oscillator, and a PL011 console. Phandles are assigned explicitly (intc = 1,
 * osc = 2) so the append-only builder can emit interrupt-parent / clocks
 * without a fixup pass.
 *
 * Only the pieces the emulator actually serves are advertised: no virtio, PCI,
 * or platform-bus, so the guest never binds a driver for a device that will
 * fault. Node order follows the reference .dts; the kernel walks it by name and
 * compatible, not order.
 */

#include "oemu/virt_dtb.h"

#include "oemu/check.h"

#include <stdio.h>

/* The machine layout boot() assembles; the tree must describe it exactly. */
#define VIRT_UART_BASE 0x09000000ULL
#define VIRT_UART_SIZE 0x00001000ULL
#define VIRT_GIC_DIST  0x08000000ULL
#define VIRT_GIC_CPU   0x08010000ULL
#define VIRT_GIC_SIZE  0x00010000ULL
#define VIRT_PL011_CLK 0x017D7840U /* 25 MHz, the fixed-clock the reference uses */

#define PHANDLE_INTC 1U
#define PHANDLE_OSC  2U

static const char *const VIRT_DEFAULT_CMDLINE =
    "console=ttyAMA0 earlycon=pl011,0x9000000 panic=-1";

/* reg for a #address-cells=2 / #size-cells=2 parent: hi addr, lo addr, hi size,
 * lo size, as four big-endian u32 cells. */
static oemu_status prop_reg64(oemu_fdt *fdt, const char *name, uint64_t addr, uint64_t size) {
  const uint32_t cells[4] = {(uint32_t)(addr >> 32U), (uint32_t)addr, (uint32_t)(size >> 32U),
                             (uint32_t)size};
  return oemu_fdt_prop_cells(fdt, name, cells, 4U);
}

/* GIC interrupt-specifier: <type, number, flags>. */
static oemu_status prop_irq(oemu_fdt *fdt, const char *name, const uint32_t *spec,
                            size_t count) {
  return oemu_fdt_prop_cells(fdt, name, spec, count);
}

static oemu_status emit_chosen(oemu_fdt *fdt, const oemu_virt_dtb_params *p) {
  oemu_status st = oemu_fdt_begin_node(fdt, "chosen");
  if (st != OEMU_OK) {
    return st;
  }
  st = oemu_fdt_prop_str(fdt, "bootargs",
                         (p->cmdline != NULL) ? p->cmdline : VIRT_DEFAULT_CMDLINE);
  if (st != OEMU_OK) {
    return st;
  }
  st = oemu_fdt_prop_u64(fdt, "linux,initrd-start", p->initrd_start);
  if (st != OEMU_OK) {
    return st;
  }
  st = oemu_fdt_prop_u64(fdt, "linux,initrd-end", p->initrd_end);
  if (st != OEMU_OK) {
    return st;
  }
  st = oemu_fdt_prop_str(fdt, "stdout-path", "/pl011@9000000");
  if (st != OEMU_OK) {
    return st;
  }
  return oemu_fdt_end_node(fdt);
}

static oemu_status emit_root_props(oemu_fdt *fdt) {
  static const uint32_t two = 2U;
  oemu_status st = oemu_fdt_prop_u32(fdt, "#address-cells", two);
  if (st != OEMU_OK) {
    return st;
  }
  st = oemu_fdt_prop_u32(fdt, "#size-cells", two);
  if (st != OEMU_OK) {
    return st;
  }
  st = oemu_fdt_prop_str(fdt, "compatible", "linux,dummy-virt");
  if (st != OEMU_OK) {
    return st;
  }
  st = oemu_fdt_prop_str(fdt, "model", "oemu-virt");
  if (st != OEMU_OK) {
    return st;
  }
  /* interrupt-parent is a phandle to /interrupt-controller@8000000, which
   * below declares phandle = PHANDLE_INTC. */
  return oemu_fdt_prop_u32(fdt, "interrupt-parent", PHANDLE_INTC);
}

static oemu_status emit_psci(oemu_fdt *fdt) {
  oemu_status st = oemu_fdt_begin_node(fdt, "psci");
  if (st != OEMU_OK) {
    return st;
  }
  st = oemu_fdt_prop_str(fdt, "compatible", "arm,psci-1.0");
  if (st != OEMU_OK) {
    return st;
  }
  st = oemu_fdt_prop_str(fdt, "method", "smc");
  if (st != OEMU_OK) {
    return st;
  }
  const uint32_t cpu_on = 0xC4000003U;
  const uint32_t cpu_off = 0x84000002U;
  const uint32_t cpu_suspend = 0xC4000001U;
  const uint32_t migrate = 0xC4000005U;
  st = oemu_fdt_prop_u32(fdt, "cpu_on", cpu_on);
  if (st != OEMU_OK) {
    return st;
  }
  st = oemu_fdt_prop_u32(fdt, "cpu_off", cpu_off);
  if (st != OEMU_OK) {
    return st;
  }
  st = oemu_fdt_prop_u32(fdt, "cpu_suspend", cpu_suspend);
  if (st != OEMU_OK) {
    return st;
  }
  st = oemu_fdt_prop_u32(fdt, "migrate", migrate);
  if (st != OEMU_OK) {
    return st;
  }
  return oemu_fdt_end_node(fdt);
}

static oemu_status emit_memory(oemu_fdt *fdt, const oemu_virt_dtb_params *p) {
  char name[32];
  (void)snprintf(name, sizeof(name), "memory@%llx", (unsigned long long)p->ram_base);
  oemu_status st = oemu_fdt_begin_node(fdt, name);
  if (st != OEMU_OK) {
    return st;
  }
  st = oemu_fdt_prop_str(fdt, "device_type", "memory");
  if (st != OEMU_OK) {
    return st;
  }
  st = prop_reg64(fdt, "reg", p->ram_base, p->ram_size);
  if (st != OEMU_OK) {
    return st;
  }
  return oemu_fdt_end_node(fdt);
}

static oemu_status emit_cpus(oemu_fdt *fdt) {
  static const uint32_t one = 1U;
  static const uint32_t zero = 0U;
  oemu_status st = oemu_fdt_begin_node(fdt, "cpus");
  if (st != OEMU_OK) {
    return st;
  }
  st = oemu_fdt_prop_u32(fdt, "#address-cells", one);
  if (st != OEMU_OK) {
    return st;
  }
  st = oemu_fdt_prop_u32(fdt, "#size-cells", zero);
  if (st != OEMU_OK) {
    return st;
  }
  st = oemu_fdt_begin_node(fdt, "cpu@0");
  if (st != OEMU_OK) {
    return st;
  }
  st = oemu_fdt_prop_str(fdt, "device_type", "cpu");
  if (st != OEMU_OK) {
    return st;
  }
  st = oemu_fdt_prop_str(fdt, "compatible", "arm,cortex-a53");
  if (st != OEMU_OK) {
    return st;
  }
  st = oemu_fdt_prop_str(fdt, "enable-method", "psci");
  if (st != OEMU_OK) {
    return st;
  }
  st = oemu_fdt_prop_u32(fdt, "reg", zero);
  if (st != OEMU_OK) {
    return st;
  }
  st = oemu_fdt_end_node(fdt);
  if (st != OEMU_OK) {
    return st;
  }
  return oemu_fdt_end_node(fdt);
}

static oemu_status emit_intc(oemu_fdt *fdt) {
  static const uint32_t three = 3U;
  oemu_status st = oemu_fdt_begin_node(fdt, "interrupt-controller@8000000");
  if (st != OEMU_OK) {
    return st;
  }
  st = oemu_fdt_prop_str(fdt, "compatible", "arm,cortex-a15-gic");
  if (st != OEMU_OK) {
    return st;
  }
  /* reg = <dist 0x10000>, <cpuif 0x10000>: two address/size pairs. */
  const uint32_t reg[8] = {0x0U, (uint32_t)VIRT_GIC_DIST, 0x0U, (uint32_t)VIRT_GIC_SIZE,
                           0x0U, (uint32_t)VIRT_GIC_CPU,  0x0U, (uint32_t)VIRT_GIC_SIZE};
  st = oemu_fdt_prop_cells(fdt, "reg", reg, 8U);
  if (st != OEMU_OK) {
    return st;
  }
  st = oemu_fdt_prop_u32(fdt, "#interrupt-cells", three);
  if (st != OEMU_OK) {
    return st;
  }
  st = oemu_fdt_prop_empty(fdt, "interrupt-controller");
  if (st != OEMU_OK) {
    return st;
  }
  st = oemu_fdt_prop_u32(fdt, "phandle", PHANDLE_INTC);
  if (st != OEMU_OK) {
    return st;
  }
  return oemu_fdt_end_node(fdt);
}

static oemu_status emit_timer(oemu_fdt *fdt) {
  /* Non-secure physical, non-secure virtual, hyp, secure physical PPIs, as the
   * reference lists them: <1, offset, 0xf04>. PPI id = 16 + offset. */
  static const uint32_t irq[12] = {1U, 13U, 0xf04U, 1U, 14U, 0xf04U,
                                   1U, 11U, 0xf04U, 1U, 10U, 0xf04U};
  oemu_status st = oemu_fdt_begin_node(fdt, "timer");
  if (st != OEMU_OK) {
    return st;
  }
  st = oemu_fdt_prop_str(fdt, "compatible", "arm,armv8-timer");
  if (st != OEMU_OK) {
    return st;
  }
  st = prop_irq(fdt, "interrupts", irq, 12U);
  if (st != OEMU_OK) {
    return st;
  }
  st = oemu_fdt_prop_empty(fdt, "always-on");
  if (st != OEMU_OK) {
    return st;
  }
  return oemu_fdt_end_node(fdt);
}

static oemu_status emit_osc(oemu_fdt *fdt) {
  static const uint32_t one = 1U;
  oemu_status st = oemu_fdt_begin_node(fdt, "oscillator");
  if (st != OEMU_OK) {
    return st;
  }
  st = oemu_fdt_prop_str(fdt, "compatible", "fixed-clock");
  if (st != OEMU_OK) {
    return st;
  }
  st = oemu_fdt_prop_u32(fdt, "#clock-cells", one);
  if (st != OEMU_OK) {
    return st;
  }
  st = oemu_fdt_prop_u32(fdt, "clock-frequency", VIRT_PL011_CLK);
  if (st != OEMU_OK) {
    return st;
  }
  st = oemu_fdt_prop_u32(fdt, "phandle", PHANDLE_OSC);
  if (st != OEMU_OK) {
    return st;
  }
  return oemu_fdt_end_node(fdt);
}

static oemu_status emit_pl011(oemu_fdt *fdt) {
  static const uint32_t irq[3] = {0U, 1U, 4U}; /* SPI offset 1 -> IRQ 33 */
  static const uint32_t clock[2] = {PHANDLE_OSC, 0U};
  oemu_status st = oemu_fdt_begin_node(fdt, "pl011@9000000");
  if (st != OEMU_OK) {
    return st;
  }
  st = oemu_fdt_prop_str(fdt, "compatible", "arm,pl011");
  if (st != OEMU_OK) {
    return st;
  }
  st = prop_reg64(fdt, "reg", VIRT_UART_BASE, VIRT_UART_SIZE);
  if (st != OEMU_OK) {
    return st;
  }
  st = prop_irq(fdt, "interrupts", irq, 3U);
  if (st != OEMU_OK) {
    return st;
  }
  st = oemu_fdt_prop_cells(fdt, "clocks", clock, 2U);
  if (st != OEMU_OK) {
    return st;
  }
  st = oemu_fdt_prop_str(fdt, "clock-names", "uartclk");
  if (st != OEMU_OK) {
    return st;
  }
  return oemu_fdt_end_node(fdt);
}

static oemu_status emit_aliases(oemu_fdt *fdt) {
  oemu_status st = oemu_fdt_begin_node(fdt, "aliases");
  if (st != OEMU_OK) {
    return st;
  }
  st = oemu_fdt_prop_str(fdt, "serial0", "/pl011@9000000");
  if (st != OEMU_OK) {
    return st;
  }
  return oemu_fdt_end_node(fdt);
}

oemu_status oemu_virt_dtb_build(oemu_fdt *fdt, const oemu_virt_dtb_params *params) {
  OEMU_REQUIRE(fdt != NULL, "NULL oemu_fdt");
  OEMU_REQUIRE(params != NULL, "NULL oemu_virt_dtb_params");
  oemu_status st = emit_root_props(fdt);
  if (st != OEMU_OK) {
    return st;
  }
  st = emit_chosen(fdt, params);
  if (st != OEMU_OK) {
    return st;
  }
  st = emit_aliases(fdt);
  if (st != OEMU_OK) {
    return st;
  }
  st = emit_psci(fdt);
  if (st != OEMU_OK) {
    return st;
  }
  st = emit_memory(fdt, params);
  if (st != OEMU_OK) {
    return st;
  }
  st = emit_cpus(fdt);
  if (st != OEMU_OK) {
    return st;
  }
  st = emit_intc(fdt);
  if (st != OEMU_OK) {
    return st;
  }
  st = emit_timer(fdt);
  if (st != OEMU_OK) {
    return st;
  }
  st = emit_osc(fdt);
  if (st != OEMU_OK) {
    return st;
  }
  st = emit_pl011(fdt);
  if (st != OEMU_OK) {
    return st;
  }
  /* oemu_fdt_init opened the root node; close it so finish() sees a balanced
   * tree. */
  st = oemu_fdt_end_node(fdt);
  if (st != OEMU_OK) {
    return st;
  }
  st = oemu_fdt_finish(fdt);
  if (st != OEMU_OK) {
    return st;
  }
  return OEMU_OK;
}
