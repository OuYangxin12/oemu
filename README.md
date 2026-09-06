# oemu

An AArch64 emulator built on a pure C11 core with a local GoogleTest harness,
with two modes over the same decode/exec engine:

- **`oemu run`** — the user-mode interpreter: a static AArch64 ELF executes at
  EL0 against a host-provided syscall layer. Fully working today; kept as the
  regression facade as the system stack grows underneath it.
- **`oemu boot`** — full-system mode: a `virt`-like machine that raises the
  shared engine through exception levels, an MMU and real devices, ending with
  a booted Linux. Under development; the committed plan is
  [`docs/roadmap-full-system.md`](docs/roadmap-full-system.md) (M1–M5, with M1
  and the register/exception layers of M2 merged).

Production code is C11 with hidden visibility and no C++ dependency. Only the
test translation units are C++17, linking against the C library through
`extern "C"` declarations.

## Emulation target

ARM is not one instruction set but a family: several architecture generations,
three profiles (A/R/M), and three distinct encodings (A32, T32/Thumb, A64).
Picking a subset up front is what keeps the decoder tractable, so oemu commits
to exactly one encoding and a deliberately shaped privilege model:

| Axis | Choice |
| --- | --- |
| Architecture | ARMv8-A (the 64-bit baseline, v8.0) |
| Profile | A — Application |
| Encoding | **A64 only** — fixed 32-bit instructions |
| Privilege, user mode | EL0 only |
| Privilege, system mode | **EL3 + EL1 + EL0** — no EL2, no VHE, no secure world |
| Register width | 64-bit `X0`–`X30`, `SP`, `PC`, `NZCV` |
| Memory model, user mode | little-endian, flat address space |
| Memory model, system mode | static physical map, stage-1 MMU added in M3, no unmap |

System mode's privilege shape is a feature-boundary statement, not a shortcut:
`ID_AA64PFR0_EL1` reports EL2 as not implemented and AArch64-only support at
each level, so a Linux kernel skips every EL2/KVM code path on its own. EL3 is
a modelled state machine, never executed code — PSCI is emulated by
intercepting `SMC #0` at exception entry (the same approach as QEMU's in-model
PSCI). The machine layout mirrors QEMU
`-machine virt` address conventions — RAM at `0x40000000`, GIC distributor at
`0x08000000`, GIC CPU interface at `0x08010000`, PL011 UART0 at `0x09000000` —
so a guest needs a modified defconfig, never a patched kernel.

### In scope

- The A64 base integer instruction set: data processing (immediate, register,
  shifted/extended), loads and stores including the addressing modes and
  pair/exclusive forms, branches, conditional selects, and the `NZCV` flag
  semantics.
- User mode: `SVC`-based system-call entry, so a static binary can make
  progress against a host-provided syscall layer.
- System mode (per the [roadmap](docs/roadmap-full-system.md)): exception
  levels, the AArch64 vector table and precise exception delivery; a
  table-driven system-register bank; the stage-1 MMU with a strict
  "TLB is only ever faster, never different" contract; `virt`-like devices
  (PL011, GICv2, arch timer), PSCI, a device-tree builder and the AArch64
  Image loader; cooperative single-threaded vCPUs for `--smp N`.

### Out of scope

Deliberately excluded, because each one multiplies decoder and state size
without changing the core design:

- **A32 and T32/Thumb.** A64 is a separate encoding; supporting the 32-bit
  ones means a second decoder, not an extension of the first. Interworking
  (`AArch32` execution state) is therefore absent too.
- **EL2, VHE, and the secure world.** The non-secure EL3+EL1+EL0 triangle is
  the whole model; virtualisation and TrustZone would double the exception and
  routing state for features no target guest uses.
- **Optional extensions:** AdvSIMD/NEON, floating point, SVE/SVE2, SME,
  Crypto, Pointer Authentication, MTE, and the v8.1+ / v9 feature increments.
  The decoder honestly reports `UNSUPPORTED` for SIMD; the advertised feature
  ID registers (FP=0, SIMD=0, LSE=0, PA=0, PMU=0) keep guests inside the
  boundary. FP/SIMD is the tracked horizon (M6).
- **Self-modifying code and cache maintenance semantics.** Instruction-cache
  coherency operations are accepted and ignored rather than modelled.

Anything outside the subset must be reported as an `oemu_status` decode or
unimplemented-instruction error, never silently executed as something else.
Extensions can be added later; none of them may be assumed present today.

### Invariants that survive every phase

These hold in both modes and are what each roadmap phase must keep unbroken:

1. Pure C11 production code, no C++ dependency, and **zero allocation on the
   step path**.
2. **Precise exception semantics:** when an exception is delivered, no
   architectural state has advanced past the triggering instruction.
3. All allocation goes through the `oemu_allocator` seam, so every OOM path is
   reachable by a test.
4. `make test`, `make asan`, `make tsan`, coverage and `-Werror` stay green.

## Decoding

`src/decode/decode.c` turns one 32-bit word plus the current `PC` into an
`oemu_insn`. It is structured to follow the decode tree in the ARM Architecture
Reference Manual rather than to be short, because that tree is the only complete
list of which bit patterns are legal; a decoder organised any other way has to
re-derive those constraints and will get some of them wrong.

Two decisions in that file are load-bearing and easy to undo by accident:

**Reserved does not mean plausible.** An encoding whose reserved bits are
non-zero, or whose combination of otherwise-valid fields the architecture leaves
unallocated, returns `OEMU_ERR_DECODE` — undefined encoding, the guest is wrong.
A valid instruction the subset excludes (anything SIMD/FP, the
pointer-authentication branches) returns `OEMU_ERR_UNSUPPORTED` — the guest is
fine, oemu is incomplete. Collapsing the two would let an absent feature look
like a corrupt binary, which is the wrong diagnosis to act on. A few
system-mode instructions — `ERET`, `WFI`/`WFE`, the `SYS`/`MRS`/`MSR` group —
decode as their own opcodes even though the user-mode executor refuses them at
dispatch: the decode tree is shared by both modes, and refusing at execution
keeps the user-mode contract intact while the system stack grows in.

**`0b1111` means *always*, not *never*.** A four-bit condition field's all-ones
value is not a sixteenth condition but the `NV` mnemonic, and in AArch64 it
behaves exactly like `AL`. Reading it as "never" is a silent misexecution rather
than a crash, so the decoder keeps the raw encoding (`OEMU_COND_NV`) and
`oemu_regs_cond_holds` treats it as taken. The distinction stays visible instead
of being folded away at decode time.

Register fields use 5 bits, and value `0b11111` means the zero register or the
stack pointer depending on the instruction form. That distinction is not
derivable from `oemu_insn` alone, so the decoder records it as the
`rn_is_sp_form` and `rd_is_sp_form` flags, and `oemu_regs` provides two accessor
families (`oemu_regs_read` and `oemu_regs_read_sp_form`) rather than guessing.

Tests are driven by a golden corpus of 198 instruction words obtained from
`llvm-mc -triple=aarch64 -show-encoding`, so the expected mnemonic of every
corpus entry comes from an independent assembler and not from whoever wrote the
decoder, plus a pseudo-random sweep over the encoding space that checks the
invariants which must hold for *any* input: a failed decode leaves the output
untouched, register fields never exceed 31, and only PC-relative forms depend on
`PC`.

## Execution

`src/exec/exec.c` is the fetch–decode–dispatch loop, and both modes run it. In
user mode one `oemu_cpu` is one single-threaded AArch64 core at EL0: the
`oemu_regs` state plus the two pieces an instruction can observe that the
register module deliberately does not model -- the exclusive-access monitor and
`TPIDRUR_EL0`. The struct is not opaque so it lives on the stack, and **no step
ever allocates**, which is what lets the executor be driven from a leak-checked
test fixture. `oemu_exec_step` runs exactly one instruction (optionally handing
back the decoded `oemu_insn` even when it then faults, so a caller can show the
offending instruction); `oemu_exec_run` loops it until the guest exits, a
budget is spent, or an error surfaces. Decoding is not reimplemented here -- the
executor calls `oemu_decode`, so the two agree by construction.

Since M1b the executor has no direct dependency on any memory implementation:
every fetch, read, write and validation goes through the `oemu_memops` bus seam
(`include/oemu/memops.h`). The user-mode facade wraps `oemu_memory` into those
ops; a system-mode machine hands the executor its `oemu_aspace` ops instead,
which is how the same loop will one day hit MMIO devices. Exception delivery
(`src/exc`) and the banked system-register state (`src/sysreg`) sit beside this
loop for the system-mode vCPU; in user mode they stay dormant.

**Faults are precise, including the awkward instructions.** When a step returns
`OEMU_ERR_FAULT` (bad address, bad permission, misaligned fetch, `BRK`/`HLT`) or
`OEMU_ERR_UNSUPPORTED`, the `PC` still points at the faulting instruction and no
architectural register has moved. That holds even for instructions that touch
several locations -- `STP` writeback, `LDP` post-index, `LDPSW` -- which is why
every memory reference is validated before the first commit and multi-transfer
forms stage through locals rather than writing one half and failing on the
other. A partially-applied store is the kind of bug that never shows up until a
guest misbehaves hours later, so the design refuses to allow it.

**Only the EL0-visible system registers are honoured** in user mode.
`MRS`/`MSR` accept a small whitelist -- `NZCV`, `SP_EL0`, `TPIDRRO_EL0`,
`TPIDRUR_EL0`, and a read of `CurrentEL` that answers EL0. Any other register,
in particular anything at EL1+, is `OEMU_ERR_UNSUPPORTED` and the destination
register is left exactly as it was: a refusal must not clobber state. System
mode inverts this deliberately: the full banked state lives in `src/sysreg`'s
table-driven register bank, and an access to a register the model does not
implement injects an Undefined exception -- the architectural behaviour a guest
kernel can handle -- instead of returning an error to the host.

**The syscall surface is the four calls a static benchmark actually needs**
(`src/core/sysenv.c`), at their honest Linux AArch64 numbers so a guest built
against them also runs on real hardware: `exit` (93), `exit_group` (94),
`write` (64) and `clock_gettime` (113). Guest buffer pointers are validated
against mapped, readable regions, so a wild pointer returns `-EFAULT` instead of
corrupting the host; `write` only accepts fds 1 and 2; unknown numbers come back
as `-ENOSYS`; failures are negative errnos in `x0`, the way Linux reports them.

### Limitations, stated plainly

Each of these is a real gap, not a TODO comment, and each is invisible only for
as long as the assumptions below hold:

- **No memory-ordering model.** `LDXR`/`STXR` reservations and `STLR` store-release
  are tracked, but the acquire-release *ordering* is not: on one in-order core no
  barrier semantics are observable, so the pair behaves correctly by accident of
  single-threading. That accident is protected by design: system mode keeps its
  vCPUs cooperative on one host thread (no locks, no data races between cores),
  so the argument keeps holding. A future multithreaded host would need real fences.
- **SP alignment is not checked.** A misaligned SP-based memory access proceeds
  rather than raising the `SPAlignmentFault` a real core would take. System mode
  closes this in M3, where data alignment is enforced and misalignment is
  delivered as an Alignment fault.
- **ELF loading covers static `ET_EXEC` only.** See the next section: the loader
  maps a non-PIE AArch64 executable's `PT_LOAD` segments. A position-independent
  `ET_DYN` image (which needs relocations applied) and dynamic linking are
  deliberately `OEMU_ERR_UNSUPPORTED`, not half-implemented.
- **Wrapped bitfield aliases are unverified.** Plain `UBFM`/`BFM`/`SBFM` (and
  `EXTR`) are exercised; the `UBFIZ`/`SBFIZ` spellings an assembler emits for the
  wrapped `msb < lsb` encodings are decoded identically but that equivalence was
  not checked against a reference, so those forms are treated as unverified.

## Loading an ELF image

`src/elf/elf.c` turns a static AArch64 executable into mapped guest memory.
`oemu_elf_load` parses the file as little-endian bytes at explicit offsets -- it
never casts the image to an `Elf64_*` struct, which would be an alignment and
strict-aliasing violation and would let the host's byte order change what a guest
image means -- validates that it is exactly what the emulator runs, then maps
each `PT_LOAD` segment through `oemu_memory_map_image` and returns the entry
point and the loaded address range.

**Validate everything before touching memory.** Every check that does not need to
mutate `oemu_memory` -- header identity, the whole program-header table's bounds,
each segment's size consistency, segment overlap, region-table capacity -- runs
in a first pass; only a fully validated segment set is mapped in a second pass.
`oemu_memory` has no unmap, so a rejected image would otherwise leave the guest
address space half-loaded. The one failure that can still arrive mid-map is the
allocator (`OEMU_ERR_NO_MEMORY`); the contract is then to dispose the whole model
and retry, which the loader makes safe by freeing its scratch before returning.

**`oemu_memory_map_image` exists because the loader must be able to install
read-only text.** A file-backed segment is copied at page-installation time, not
as a guest store, so the region can carry the exact permissions it will run under
-- including `R`-without-`W`. Populating it through the ordinary
`oemu_memory_write_bytes` would demand `WRITE`, which is exactly what the loader
must not require of `.text`. The span past the file slice stays zero, so a `.bss`
tail needs no separate handling.

## Requirements

| Tool | Purpose | Notes |
| --- | --- | --- |
| CMake ≥ 3.20 | build system | 3.23+ recommended (needed to skip a conda prefix) |
| Ninja | generator used by the presets | any generator works if you invoke CMake directly |
| GCC or Clang | C11 + C++17 | GCC 13 / Clang 18 verified |
| GoogleTest ≥ 1.11 | test framework | `sudo apt install libgtest-dev libgmock-dev` |
| lcov | HTML coverage report | optional; `make coverage-summary` needs only gcov |
| clang-format, clang-tidy | style and static analysis | optional |

## Quick start

```bash
make test          # configure, build and run the whole suite
make run GUEST=path/to/prog.elf   # run a static AArch64 ELF under the CLI
make bench-exec    # end-to-end interpreter throughput (see bench/README.md)
make help          # list every target
```

### Running a binary

The `oemu run` subcommand runs a static AArch64 `ET_EXEC` image:

```bash
./build/debug/bin/oemu run prog.elf            # run to completion, exit status
./build/debug/bin/oemu run prog.elf --max-insns 1000000
./build/debug/bin/oemu run prog.elf | diff - expected.txt   # stdout passes through
```

It prints nothing on a clean run and exits with the guest's own exit code. The
process exit status is `1` (load/read/run failure), `2` (bad usage) or `3`
(instruction budget spent) when the guest never reached `exit`; otherwise it is
whatever the guest asked for. `make run` and `make bench-e2e` are thin wrappers
around `oemu run $(GUEST)`; `GUEST` defaults to the freestanding guest under
`bench/guest/`, so `make bench-guest && make bench-e2e` boots that guest (and
skips cleanly when no AArch64 linker is available).

Behind the scenes that is just CMake:

```bash
cmake --preset debug
cmake --build build/debug
ctest --preset debug
```

The system-mode entry point, `oemu boot -kernel Image ...`, is not shipped yet;
it arrives with the register/vCPU work (M2) and the device stack (M4). Until
then the roadmap document is the source of truth for its planned flags and
behaviour.

## Layout

```
include/oemu/         Public headers. Consumers see only these.
  macros.h            extern "C" guards and attribute helpers
  status.h            oemu_status result codes
  allocator.h         pluggable allocator (the test seam)
  check.h             OEMU_REQUIRE fatal contract macro
  buffer.h            worked example: growable byte buffer
  regs.h              AArch64 register state: X0-X30, SP, PC, NZCV
  decode.h            A64 decoder: decoded instruction record and status codes
  memory.h            flat guest address space: map, alias, read/write/fetch
  sysenv.h            the four-call SVC surface the guest runs against
  exec.h              the CPU: register bag plus the step/run interpreter loop
  elf.h               load a static AArch64 ET_EXEC image into guest memory
  memops.h            the bus seam: fetch/read/write/validate over an opaque ctx
  aspace.h            guest physical address space: RAM regions + MMIO regions
  device.h            MMIO device callback contract (callbacks never allocate)
  machine.h           machine container: RAM block, device table, power/reset events
  sysreg.h            banked system-register state for the system-mode CPU
  exc.h               exception delivery: entry sequence, syndrome builders, ERET
src/
  core/               status, version, allocator, check, sysenv
  buffer/
    buffer.c          implementation
    buffer_internal.h internal interface, exposed for white-box tests
  regs/
    regs.c            register state, condition codes, flag derivation
    regs_internal.h   pure helpers: truncation, cond table, AddWithCarry
  decode/
    decode.c          A64 instruction decoder, mirroring the ARM ARM decode tree
    decode_internal.h bit primitives: extract, sign-extend, rotate, mask expand
  memory/
    memory.c          region table, permission checks, endian-aware accessors
    memory_internal.h internal region layout and validation helpers
  exec/
    exec.c            fetch-decode-dispatch loop over memops, precise faults
    exec_internal.h   pure helpers: shifts, extends, NZCV, bit reversal, bitfield
  aspace/
    aspace.c          physical region table: RAM dispatch, MMIO fan-out
    aspace_internal.h pure containment/overlap helpers, mirroring memory's
  machine/
    machine.c         machine init, RAM mapping, poweroff/reset event record
  sysreg/
    sysreg.c          table-driven system registers: encoding, reset, RES0 masks
    sysreg_internal.h pure helpers: encoding lookup, mask composition
  exc/
    exc.c             exception take/return: SPSR/ELR/ESR/FAR, vector selection
    exc_internal.h    pure syndrome and vector-table helpers
  elf/
    elf.c             static AArch64 ET_EXEC loader: validate, then map PT_LOAD
    elf_internal.h    pure helpers: little-endian readers, segment validation
  main.c              the `oemu` CLI: load an ELF from disk, map a stack, run it
tests/
  support/            shared test doubles (tracking + failing allocators)
  unit/               one test file per module
cmake/
  CompilerWarnings.cmake  central warning set
  Sanitizers.cmake        ASan/UBSan/TSan wiring
  Coverage.cmake          gcov instrumentation + report targets
  GcovSummary.cmake       text coverage summary, no lcov required
AGENTS.md             Baseline instructions for coding agents.
.dsh/skills/          Task-specific agent skills; see .dsh/README.md
```

## Coding agents

`AGENTS.md` holds the always-loaded project rules. Four skills under
`.dsh/skills/` cover the recurring workflows: `oemu-run-tests`,
`oemu-build-configs`, `oemu-add-c-module` and `oemu-ci-workflow`. See
[`.dsh/README.md`](.dsh/README.md) for the format and discovery rules.

These are useful documentation for humans too — the build and test skills record
the environment-specific workarounds and their reasons.


## Build configurations

Each preset builds into its own tree under `build/`, so they coexist.

| Preset | Purpose |
| --- | --- |
| `debug` | unoptimised, warnings as errors |
| `release` | `RelWithDebInfo`, tests still enabled |
| `asan` | AddressSanitizer + UndefinedBehaviorSanitizer |
| `tsan` | ThreadSanitizer |
| `coverage` | gcov instrumentation |
| `clang` | debug build on the Clang toolchain |

```bash
make            # debug (default target is `test`)
make asan       # run the suite under ASan + UBSan
make tsan
make release
make clang
make coverage-summary   # text coverage, gcov only
make coverage           # HTML report, needs lcov
```

## Running tests

`gtest_discover_tests` registers every `TEST()` as its own CTest case, so
failures name the exact case and selections are precise.

```bash
ctest --test-dir build/debug -N                  # list all cases
ctest --test-dir build/debug -R Buffer           # run by name regex
ctest --test-dir build/debug -L whitebox         # run by label
ctest --test-dir build/debug -LE death           # skip the slow death tests
ctest --test-dir build/debug --rerun-failed      # only what failed last time

make test-fast     # same as -LE death
make test-death    # only the death tests
make retest        # rerun failures
```

Labels in use: `unit` on everything, plus `whitebox` and `death` where
applicable.

A test binary can also be run directly for gtest's own flags:

```bash
./build/debug/bin/test_buffer --gtest_filter='BufferOom.*' --gtest_brief=1
```

## Writing tests

Add a `.cpp` file under `tests/unit/` and register it in `tests/CMakeLists.txt`:

```cmake
oemu_add_test(test_mymodule
  SOURCES unit/test_mymodule.cpp
  LABELS unit
)
```

Options: `INTERNAL` grants access to `src/<module>/<module>_internal.h`;
`LABELS` sets the CTest labels.

### Making C code testable

Four conventions carry the whole harness. They matter more than the CMake
plumbing, and they are what will keep the A64 decoder and CPU state testable as
they grow.

**1. Public headers must be C++-safe.** Every public header wraps its
declarations in `OEMU_BEGIN_DECLS` / `OEMU_END_DECLS` (`extern "C"`), otherwise
the C++ test would emit mangled symbol references and fail to link. Keep
designated initialisers, VLAs, `restrict` and `_Generic` out of public headers:
C++ cannot parse them.

**2. Internal logic goes in `<module>_internal.h`, not `static`.** A `static`
function is unreachable from a test. Rather than making tests
`#include "buffer.c"`, each module publishes its internals in a private header
that only the module and its tests include. See
`src/buffer/buffer_internal.h` and `tests/unit/test_buffer_internal.cpp`, which
tests the capacity growth policy directly, including overflow behaviour near
`SIZE_MAX`.

**3. Allocation goes through a seam.** gmock cannot mock C free functions — it
needs virtual dispatch. So the library routes all allocation through
`oemu_allocator`, and tests swap the implementation:

- `TrackingAllocator` counts allocations and frees, and detects leaks;
- `FailingAllocator` fails the Nth allocation, making out-of-memory branches
  reachable.

```cpp
TEST(BufferOom, AppendReportsGrowthFailure) {
  oemu_test::FailingAllocator failing(oemu_test::FailingAllocator::kNever);
  oemu_buffer buf{};
  ASSERT_EQ(OEMU_OK, oemu_buffer_init(&buf, 0));

  failing.set_fail_on_call(1);
  EXPECT_EQ(OEMU_ERR_NO_MEMORY, oemu_buffer_append_str(&buf, "data"));
  EXPECT_EQ(0u, oemu_buffer_len(&buf));

  oemu_buffer_dispose(&buf);
}
```

Both install themselves on construction and restore the previous allocator on
destruction, so a failing test cannot leak its override into the next one.

**4. Fatal contracts are verified with death tests.** `OEMU_REQUIRE` aborts on
programming errors. gtest runs each death test in a forked child, so the abort
is observed without killing the test binary — a capability plain C assertion
frameworks lack. `tests/unit/test_check.cpp` selects the `threadsafe` death-test
style, which avoids spurious leak reports from the aborted child under ASan.

Recoverable failures should return `oemu_status` instead, which keeps them
assertable in ordinary tests.

## Coverage

```bash
make coverage-summary   # per-file text summary; needs only gcov
make coverage           # HTML report in build/coverage/coverage-html; needs lcov
```

Current, by new line coverage: **decode 99%, regs 100%, exec 96%, elf 96%,
sysenv 94%, main 84%, buffer 95%, aspace 99%, machine 100%, sysreg 98%, exc
82%; TOTAL 96% of lines (2747/2841).**
The uncovered exec and elf lines are defensive arms a correct caller never
produces: for exec, `operand2`'s extended/none operand kinds, the dispatch
`default:` (`"a newer decoder cannot outdate this switch"`), and the
`INT64_MIN / -1` divide edge; for elf, the single combined `status` guard after
each batched little-endian read, which cannot fail once the whole header table has
been bounds-checked. `main.c` is the CLI entry point, driven end-to-end by the
forking `test_cli` (which runs the real binary); its uncovered lines are the rarer
error arms -- allocation failure, a faulting or unsupported guest instruction, a
stack map that collides -- not the happy path or the exit-code contract, which the
subprocess tests cover. These read `#####` precisely because they are hard to
reach on purpose, not because they are untested. `exc` sits at 82% because its
uncovered lines are `oemu_exc_ec_name` arms for syndrome classes no test has
forced yet (SError, SMC/HVC, trapped FP, breakpoints) plus one
undefined-injection wrapper; they become reachable as the vCPU work (M2c)
drives real exception sources into the module.

One tooling caveat so the number is read correctly: `make coverage-summary`
currently omits `src/memory/memory.c` -- `file(STRINGS)` mis-parses that one
`.gcov`, so the script's TOTAL understates by memory's lines. The figure above
adds memory back from a direct `gcov` run (98% of 141 lines); it is a real
measurement, not a guess.

One wrinkle worth knowing if you add a function that terminates the process:
`abort()` prevents the gcov runtime from writing its counters, so such a function
reports 0% even when death tests cover it thoroughly. `src/core/check.c` calls
`__gcov_dump()` before aborting, guarded by `OEMU_COVERAGE` — a macro
`cmake/Coverage.cmake` has to define because GCC provides none for `--coverage`.

## Code quality

```bash
make format         # apply clang-format
make format-check   # fail on any deviation
make tidy           # clang-tidy over the C sources
make compile-db     # symlink compile_commands.json for clangd
```

## CI

`.github/workflows/ci.yml` runs the same commands as local development: a
matrix over `debug`, `clang`, `release` and `asan`, plus separate coverage and
lint jobs.

## Notes and gotchas

Two environment-specific problems are already handled; both cost real debugging
time to find.

**An active conda environment shadows the system GoogleTest.** `CONDA_PREFIX`
sits on `CMAKE_PREFIX_PATH`, so `find_package(GTest)` picks up conda's older
build (1.11) instead of the system one (1.14), potentially against a different
C++ runtime. The top-level `CMakeLists.txt` adds that prefix to
`CMAKE_IGNORE_PREFIX_PATH`; set `-DOEMU_IGNORE_CONDA_PREFIX=OFF` to opt out.
The configure output names the GoogleTest that was selected.

**CTest labels must be a single composite label.** CMake flattens the
`PROPERTIES` argument of `gtest_discover_tests` into a semicolon-separated list,
so a genuine multi-value `LABELS` property cannot survive: the extra values get
misread as further property names and silently dropped. Because `ctest -L`
matches by regex, `oemu_add_test` joins the components into one label
(`unit.death`), which keeps `-L unit`, `-L death` and `-LE death` all working.

**ThreadSanitizer aborts on modern kernels.** Linux 6.x (including WSL2) uses
more ASLR entropy than TSan's shadow-memory layout expects, so every binary dies
with `FATAL: ThreadSanitizer: unexpected memory mapping`. `Sanitizers.cmake`
detects the TSan build and routes the test binaries through `setarch -R` via the
`CROSSCOMPILING_EMULATOR` target property — which matters because
`gtest_discover_tests` runs each binary at discovery time too, not just during
the test run. The alternative is `sudo sysctl vm.mmap_rnd_bits=28`.

**A green coverage run that reports nothing means gcov does not match the
compiler.** On this machine `/usr/bin/cc` resolved to GCC 10 while
`/usr/bin/gcov` was 13.3; gcov cannot parse a profile format from a newer
compiler, and every per-TU invocation failed -- slowly, about a minute each,
with zero output -- so the summary printed "no project lines instrumented" and
still exited 0. `cmake/Coverage.cmake` now prefers `gcov-<major>` matching the
configured compiler, for both the summary and lcov's `--gcov-tool`, and
`cmake/GcovSummary.cmake` hard-fails when no `.gcov` files were produced
instead of reporting an empty run. The gcov sweep itself is fanned out through
`xargs -P` rather than one file at a time. If you override the toolchain, keep
the pair matched: `cmake -B build/coverage -U OEMU_GCOV` re-runs the lookup
after a compiler change.

Other details worth knowing:

- C-only warnings such as `-Wstrict-prototypes` are guarded with
  `$<COMPILE_LANGUAGE:C>`; without that GCC warns on every C++ test file.
- ASan and TSan are mutually exclusive; `Sanitizers.cmake` rejects the
  combination at configure time.
- UBSan runs with `-fno-sanitize-recover=all` so undefined behaviour actually
  fails the test run instead of only printing.

## Using GoogleTest from source instead

To pin a specific version rather than use the system package, replace the
`find_package(GTest REQUIRED)` call in `tests/CMakeLists.txt` with the
`FetchContent` block documented at the bottom of that file.
