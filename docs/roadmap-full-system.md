# Roadmap: from user-mode interpreter to full-system emulator

Status: approved plan, 2025. This document is the committed form of the
roadmap; phases are merged in order and each ends green under
`make test` and `make asan`.

## End goal

`oemu boot -kernel Image -initrd rootfs.cpio` boots single-core AArch64
Linux to a serial shell on a self-built `virt`-compatible machine; a guest
`poweroff` reaches the emulator through PSCI `SYSTEM_OFF` and oemu exits
with code 0. `--smp N` starts N cores.

The machine layout deliberately mirrors QEMU `-machine virt` address
conventions — RAM at `0x40000000`, GIC distributor at `0x08000000`, GIC CPU
interface at `0x08010000`, PL011 UART0 at `0x09000000` — so the guest
needs a modified defconfig, never a patched kernel.

## Invariants that survive every phase

These are what separate oemu from a toy emulator; each phase must keep
them, unbroken:

1. Pure C11 production code, no C++ dependency, **zero allocation on the
   step path**.
2. Precise exception semantics: when an exception is delivered, no
   architectural state has advanced past the triggering instruction.
3. All allocation through the `oemu_allocator` seam, so every OOM path is
   reachable by a test.
4. `make test`, `make asan`, `make tsan`, coverage and `-Werror` stay
   green.

## Locked decisions

These were settled during planning; phases below assume them and do not
re-open them.

| # | Decision | Why |
| --- | --- | --- |
| D1 | **Dual-mode core.** `oemu run` (user-mode) is kept as a regression facade; system mode is a new stack on top of shared regs/decode/exec. | The existing ~2400-line test suite is the safety net; user mode becomes a "degenerate machine" (identity mapping + sysenv), not an abandoned branch. |
| D2 | **Cooperative single-host-thread vCPUs**: fixed instruction quantum, WFI/WFE forces a yield. | No locks, the no-allocation-during-step invariant survives, memory ordering is trivially satisfied within one thread (the README's existing argument keeps holding); easier to test and reproduce than host threads. |
| D3 | **EL model: EL3 + EL1 + EL0. No EL2, no VHE, no TrustZone** (non-secure world only). | With EL2 absent, `ID_AA64PFR0.EL = 0b0001` and Linux skips all EL2/KVM paths automatically. EL3 is a modelled state machine, not executed code (D4). |
| D4 | **PSCI is emulated by intercepting `SMC #0` at exception entry**; EL3 never executes real instructions. | Avoids an assembly stub and a cross toolchain dependency. A documented deviation — QEMU's in-model PSCI does the same. |
| D5 | **Static physical address space, no unmap.** Page-table changes affect translation and the TLB, never the physical map. | Keeps the contract already written into the README, and vastly simplifies loaders and device lifetimes. |
| D6 | **FP/SIMD deferred** to the horizon (M6); boot Linux with `CONFIG_KERNEL_MODE_NEON=n`. | The decoder honestly reports `UNSUPPORTED` for SIMD; that does not block the Linux-boot mainline. |
| D7 | **Exclusive accesses to MMIO** (LDXR/STXR on a device region) raise a Data Abort (alignment fault) rather than approximate undefined behaviour. | Same philosophy as everywhere else: either architecturally precise, or an explicit error. |
| D8 | **MMU lands as a fully correct walk with no TLB first**, then a direct-mapped flush-all TLB is added to the same module. | The TLB is only allowed to be faster, never different; each layer gets its own tests. |

## Architecture overview

```
include/oemu/           src/
  machine.h      ─┐      machine/      machine model: owns aspace, device table, vCPU array, events
  vcpu.h         ─┤      vcpu/         oemu_vcpu = regs + sysregs + TLB + irq pins + quantum
  sysreg.h       ─┤      sysreg/       table-driven system registers (replaces exec.c's switch whitelist)
  exc.h          ─┤      exc/          vector-table layout, ESR/FAR/SPSR construction, ERET, routing
  aspace.h       ─┤      aspace/       physical address space: RAM regions + MMIO device regions
  device.h       ─┤      dev/          device models (pl011, gicv2, generic timer, ...)
  mmu.h          ─┤      mmu/          stage-1 walk (4 KiB granule) + TLB
  kernel.h       ─┐      kernel/       AArch64 Image loader, DTB wiring, PSCI
  fdt.h           ┘      fdt/          minimal device-tree builder
```

`src/memory/` survives as the user-mode facade, internally layered on the
new aspace. Dependencies point one way: `exec → exc/mmu/aspace`,
`machine → vcpu/devices`, `devices → aspace`. The decoder is untouched
apart from the M2 system-instruction additions.

## M1 — Address space, device bus, executor de-sysenv-ification (pure refactor)

No new semantics; the full suite stays green throughout.

**M1a.** New `include/oemu/aspace.h`, `include/oemu/device.h`,
`src/aspace/aspace.c`, `include/oemu/machine.h`, `src/machine/machine.c`.

- `oemu_aspace`: a region table reusing the containment/overlap pure
  functions from `memory_internal.h`. Regions are either
  `OEMU_REGION_RAM` (host pointer) or `OEMU_REGION_DEVICE`.
- `oemu_device_ops { ctx; read(ctx, off, size, *val); write(ctx, off,
  size, val); }` — device callbacks must never allocate; any buffering
  (e.g. a UART TX ring) is pre-allocated at device init.
- `oemu_aspace_map_ram` / `oemu_aspace_attach_device`; little-endian
  assembly reuses the existing `disassemble`/`assemble` helpers.
- `oemu_machine_init(machine, ram_base, ram_size, region_capacity)`,
  `oemu_machine_poweroff/reset` recording an `oemu_machine_event_t` the
  CLI polls. RAM is one pre-allocated `map_ram` region.

**M1b.** Injection seams in the executor.

- `exec_internal.h` gains `oemu_memops { ctx; fetch32/read/write/validate }`;
  `oemu_exec_internal_dispatch` takes `const oemu_memops *` instead of
  `oemu_memory *`. The user-mode facade wraps `oemu_memory` into memops
  (every existing `test_exec*` passes unchanged); the aspace exports its
  own memops directly.
- SVC decoupling: the `oemu_sysenv *` parameter becomes
  `const oemu_env_ops { syscall; halted; } + void *ctx`; sysenv provides
  the user-mode `env_ops`.

**Accepts:** suite green and never smaller; `oemu run` byte-identical in
behaviour (the forking CLI tests assert it); new modules follow the
`oemu-add-c-module` conventions with internal headers and OOM fixtures.

## M2 — Exception levels, vectors, system registers (MMU still off)

**Register state.** `oemu_sysregs` embedded by value in `oemu_vcpu` (no
allocation): `SP_EL0/1/2`, `ELR_ELx`, `SPSR_ELx`, `VBAR_ELx`,
`ESR/FAR_ELx`, `SCTLR/TTBR0/1/TCR/MAIR_EL1` (stored, honoured from M3),
`TPIDR_EL1`, `SPSel`, PSTATE bits (M/DAIF/IL/SS/SPSel). Table-driven
accessors `{encoding, read, write, reset}` replace the MRS/MSR switch
whitelist in `exec.c`. Unknown system-register reads **inject an
Undefined exception** in system mode (architectural behaviour);
user-mode keeps its existing error return. Constant ID registers
(`ID_AA64PFR0` with EL=`0b0001`, FP=0, SIMD=0; `ID_AA64ISAR0` LSE=0;
`ID_AA64ISAR1` PA=0; `ID_AA64DFR0` PMU=0; `MIDR/MPIDR/CLIDR/CTR_EL0…`)
define the feature boundary we advertise — Linux uses them to avoid code
paths we do not implement. Each value gets a comment citing its basis.
New `src/vcpu/` owns `oemu_vcpu` wrapping `oemu_cpu` + sysregs + memops +
pins; the public `oemu_cpu` / `oemu_exec_step` API is preserved verbatim.

**Exception machinery** (`src/exc/`): the full 16×128-byte AArch64 vector
table (slots for lower-EL-AArch32 exist but are never taken); routing is
simplified — synchronous and IRQ exceptions go to EL1 (SCR routing not
modelled, documented); `SMC`/`HVC` go to the modelled EL3 (UNDEF until
M4 installs PSCI). Entry sequence: SPSel selects `SP_ELx`, DAIF all set,
PSTATE ← {M=target, IL=1, SS=0}, SPSR saves the old PSTATE,
ELR = faulting PC, FAR/ESR built by the fault source. `ERET` restores
with privilege checks. The precise-exception contract is restated:
*delivery* does not advance state past the trigger — the fetch-fault
path stops returning an error to the host and injects an Instruction
Abort instead (user-mode facade keeps the old host-visible semantics).
ESR classes implemented: SVC AArch64 (`0x15`), Unknown (`0x00`),
Trapped FP/SIMD (`0x18`), BRK (`0x30`), Instruction Abort (`0x20/0x21`),
Data Abort (`0x24/0x25`), Alignment (`DFSC=0x01`); ISS carries ISV=1
(SAS/WnR/VAT) for plain scalar accesses, ISV=0 for pairs.

**Decoder additions:** system-encoding group split — `OEMU_OP_SYS` with
the sysreg selector fields, so DC/IC/TLBI/AT and the `ICC_*` registers
become distinguishable; WFI/WFE split out of HINT (system-mode WFI is a
scheduler yield point — NOP before M4); `ERET` decodes. Golden corpus
extended with `llvm-mc`-generated encodings; the DECODE-vs-UNSUPPORTED
dichotomy tests keep asserting the original split.

**IRQ pins:** `oemu_vcpu` gains pending-IRQ/FIQ flags refreshed at
quantum boundaries; delivery is checked between instructions and honours
DAIF masking.

**Tests:** `test_sysreg` (table, injections, RES0 masks), `test_exc`
(hand-built vector table in emulated RAM → SVC / undefined instruction →
assert full taken state and ERET round-trip), `test_vcpu` (IRQ
delivery and DAIF timing). New `tests/guest/` freestanding directory
following the optional-toolchain pattern of `bench/guest`;
`el1_smoke.S` boots at EL1, arms VBAR, SVC-traps and returns, prints a
magic byte on the fake UART, `BRK`s out. `test_boot_smoke.cpp` (label
`guest`, skipped without the cross toolchain) drives the new CLI.

**Accepts:** guest smoke test passes; `oemu boot -kernel <raw-bin>
[--entry ADDR]` maps the image at `0x40080000` and starts at EL1 with
identity mapping; `oemu run` and every prior test untouched.

## M3 — Stage-1 MMU and TLB

**M3a (walk):** EL1&0 regime, 4 KiB granule, generic `TCR.TnSZ`
(25…39 → start level 1…4), TTBR0/1 selected by VA[55]; block and page
descriptors, hardware AF update written back as a physical write
(Permission fault if the descriptor is read-only), AP/UXN/PXN
accumulated per level, Translation faults per level, Address Size
fault, identity when `SCTLR_EL1.M = 0`. Every access walks; faults are
injected through `exc` with FAR/ELR and the correct ESR ISS. Tests hand
page tables into emulated RAM and sweep fault-class × level × AP × UXN ×
block size × TnSZ; the failing allocator covers the translation path.

**M3b (TLB):** direct-mapped, 4096 entries, tagged by (VMID, nG, VA,
size); `TLBI VMALLE1IS`/`ALLE1IS`, writes to TTBR/TCR/MAIR and
`SCTLR.M` flips flush all (range invalidation deliberately unimplemented
at this stage). A random sweep asserts TLB-served results equal
walk-served results.

**Executor hookup:** the memops gain a translation layer (STP staging
translates each half); instruction fetch checks X permission; data
alignment is enforced (natural for exclusives, 8-byte for LDP/STP pairs,
all accesses when `SCTLR_EL1.A` is set) as Alignment faults.

**Accepts:** guest case `mmu_smoke.S` — build identity tables, set
`SCTLR.M`, access across a page boundary, print the UART magic, power
off. The user-mode facade bypasses translation entirely and is
unaffected.

## M4 — The virt-like machine: devices, modelled firmware, DTB, Image loader

- `src/dev/pl011.c` — DR/FR/CR/IBRD/FBRD/LCR_H/IMSC/RIS/MIS/ICR; a
  pre-allocated 16-entry TX ring; RX is an injectable test hook, later
  host stdin. TX mirrors host stdout as the console.
- `src/timer/gtimer.c` + sysreg wiring — `CNTFRQ_EL0/EL1` (default
  62.5 MHz, `-freq` flag), `CNTPCT_EL1` from the host monotonic clock ×
  frequency, `CNTP_CTL/CVAL_EL1` driving the vCPU IRQ pin; the virtual
  timer interface aliases the physical one (no EL2, offset always 0 —
  documented).
- `src/dev/gicv2.c` — distributor (CTLRD, IGROUPR, ISENABLER/ICENABLER,
  IPRIORITYR, ISACTIVER/ICACTIVER, simplified TTARGETR) plus the CPU
  interface via `ICC_IAR/EOIR/PMR/BPR/CTLR/SGI1R` sysregs whose state
  lives in the vCPU (naturally per-CPU). Fixed-table priority scan, no
  bitmap optimisation.
- `src/fw/psci.c` — intercepts `SMC #0`: `VERSION` (0x00020000),
  `CPU_ID`, `CPU_COUNT`, `CPU_AFFINITY`, `AFFINITY_INFO`, `SYSTEM_OFF`,
  `SYSTEM_RESET`, `CPU_ON` (single core: index > 0 →
  `INVALID_PARAMETERS`). `SYSTEM_OFF` → `oemu_machine_poweroff` → CLI
  exit code 0 (or the guest's requested code); reset re-boots.
- `src/fdt/fdt.c` — minimal FDT builder (structure block + strings, pure
  functions, unit-tested): `/cpus` (arm,cortex-a53, enable-method
  psci), `/psci` (conduit = "smc"), `/memory@40000000`,
  `/interrupt-controller@8000000` (arm,gic-400), `/serial@9000000`
  (arm,pl011, SPI 33), `/timer` (armv8-timer + clock-frequency),
  `/chosen`.
- `src/kernel/image.c` — AArch64 Image header parsing (magic `ARM\x64`,
  text_offset, image_size, the IPA-size requirement flags, which are
  checked and honestly refused), loaded at `0x40000000`; DTB placed
  after the image; `-append` feeds `/chosen/bootargs`.
- CLI: `oemu boot -kernel Image [--dtb file] [-m 256M] [-append "..."]
  [--smp N] [--serial FILE]`, alongside `run`.

**Accepts:** guest case `psci_off.S` — at EL1, print a string on the
PL011, `smc #0` SYSTEM_OFF → emulator exit code 0. CI keeps its
`lint`/`asan` matrix and gains a `guest` job installing
`gcc-aarch64-linux-gnu`.

## M5 — Interrupt-driven Linux: cooperative scheduling, initrd, SMP

- Machine-level cooperative scheduler for `--smp N`: quantum-bounded
  turns, PSCI `CPU_ON` records the secondary entry, all-idle parks the
  host on a real `select()` until the next timer deadline or serial
  input — never spin-burning the host.
- Interrupt wiring: arch timer → GIC PPI 27/30, IPIs as SGIs 5/6, PL011
  RX → SPI 33; GIC pending state updates the vCPU IRQ pin at quantum
  boundaries.
- Host stdin in raw mode feeds PL011 RX; `-initrd FILE` loads at a fixed
  address and registers it in `/chosen`.
- `docs/booting-linux.md`: the defconfig diff (`KERNEL_MODE_NEON=n`,
  initrd on, PMU/ptr-auth off), a minimal busybox initrd recipe, an
  `/init` that prints `BOOT OK` and powers off.
- **Accepts (the end goal):** `make boot-linux LINUX=... INITRD=...`
  reaches a shell; a CI `guest`-labelled smoke test asserts the serial
  log contains `BOOT OK` and exit code 0 (skipped when the kernel
  artifact is absent); `--smp 4` yields `nproc` == 4.

## M6 — Horizon (tracked, not scheduled)

NEON/FP (decoder subtree + 128-bit vector state), LSE atomics (flip the
ID register once implemented), EL2/VHE, a code-cache or TCG-style JIT
(the memops seam is designed for a backend swap), device plugin API.

## Cross-cutting work (rides along with each phase's PRs)

- README's "Emulation target / Out of scope" sections are rewritten in
  M1 to the dual-mode narrative and shrink phase by phase.
- Every phase runs `make test` + `make asan`; MMU/exception/arithmetic
  work requires asan. New modules target the existing ≥97% line-coverage
  bar.
- 2–4 PRs per phase, feature-branch flow; refactors never share a PR
  with features.

## Risks and counters

| Risk | Counter |
| --- | --- |
| M2 changes host-visible error returns into exception delivery | user-mode facade keeps old semantics; both modes tested separately |
| boot is unusably slow pre-TLB | M3a only runs freestanding guests; the TLB lands before any Linux run |
| a guest spin-loop starves the scheduler | forced yield every quantum + WFI yield; quantum is configurable |
| modelled (not executed) EL3 deviates from the architecture | documented in README; ID registers and DTB stay self-consistent so Linux cannot tell |
| new SYS encodings blur DECODE-vs-UNSUPPORTED | corpus extension + the existing dichotomy tests keep asserting the split |
| Linux config surface underestimated | benchmark the same defconfig under QEMU first: works there, fails here ⇒ oemu bug |

## Assumptions

- A host-runnable `gcc-aarch64-linux-gnu` cross toolchain is available
  for building test guests and the Linux Image; production code never
  depends on it.
- The Linux Image and rootfs are not stored in the repository; the CI
  boot test skips when absent.
- Order is commitment: M1 → M5 linearly; the end of M4 already yields a
  useful intermediate product (a machine that prints on its serial
  console), and Linux is M5's acceptance criterion, not a prerequisite.
