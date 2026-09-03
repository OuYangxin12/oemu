# Benchmarks

Three layers, one purpose: turn "is the fast path still fast, and is it still
right" into numbers.

| Layer | What it measures | Works today |
| --- | --- | --- |
| `corpus/` | feeds the benchmarks real compiler output, not hand-written words | yes (`make bench-corpus`) |
| `c/decode_bench.c` | `oemu_decode()` throughput on that corpus | yes (`make bench`) |
| `c/exec_bench.c` | end-to-end `oemu_exec_run()` on a self-generated guest | yes (`make bench-exec`) |
| `guest/` | a static AArch64 ELF running under oemu | prepared; needs an ELF loader, plus one apt package for linking |

The split tracks the project's phase. oemu now has a decoder, registers,
buffer, allocator, an executor and a four-syscall SVC layer, but no ELF
loader, so a benchmark that "runs a real file off disk" is still a
*destination*. The executor's own throughput, though, is measurable right now
(`exec_bench.c` builds and runs the guest itself), and it is the number an
interpreter loop pays one-for-one, every instruction, forever.

## Layer 1: the corpus (`bench/corpus/`)

Ten small freestanding C kernels (`k_*.c`), each written to push a specific
corner of the decode tree:

| Kernel | Decoder families targeted |
| --- | --- |
| `k_addsub` | ADD/SUB/S*, ADC*/SBC*, shifted- and extended-register operands |
| `k_logic` | AND(S)/ORR/ORN/EOR/EON/BIC(S), bitmask-immediate expansion |
| `k_bitfield` | SBFM/BFM/UBFM/EXTR, CLZ/CLS, RBIT/REV*, variable shifts |
| `k_movewide` | MOVZ/MOVN/MOVK chains, ADR/ADRP |
| `k_branches` | every branch form, including a switch table and indirect BLR |
| `k_ldst` | LDR/STR at all widths/signedness, post-index, SP-relative, LDP/STP, unaligned |
| `k_muldiv` | MADD/MSUB, SM{ADDL,MULH}/UM{ADDL,MULH}, UDIV/SDIV, reciprocal-multiply lowering |
| `k_csel` | CSEL/CSINC/CSINV/CSNEG, CCMP/CCMN |
| `k_hash` | realistic mixed integer workload (CRC, LCG, xorshift, FNV) |
| `k_memops` | byte-scan and bulk-copy loop shapes |
| `k_svc` | SVC #0 (plus NOP/hints) — the guest-ABI corner of the encoding space |

Each is compiled at both `-O0` and `-O2`: unoptimised code is spill-heavy
and pair-dense (what debug builds and simple guest compilers emit), optimised
code is dense and mixed (what guests are usually shipped as).

`gen.sh` cross-compiles with the host clang (`--target=aarch64-unknown-none`,
no cross toolchain, no libc, and `-mcpu=generic+nosimd` — without it clang
spills `__int128` through FP registers and expands constant stores through
NEON, putting genuinely out-of-scope encodings into the corpus), extracts
`.text` with `llvm-objcopy`, and records `manifest.txt` (bytes, words,
sha256 per blob). The blobs are
**committed** — `make bench` never requires a cross compiler — and
`make bench-corpus` regenerates them. Expect the hashes to move when the
clang version moves; that is recorded in the manifest header, not an
incident.

The blobs double as a decoder fuzzer seed corpus for whoever writes one.

## Layer 2: the decode benchmark (`bench/c/decode_bench.c`)

`make bench` runs `oemu-bench-decode`:

```
calibrate one pass -> batch sized to ~150 ms -> --trials batches -> min ns/insn
```

The minimum of trials is the estimator that survives frequency scaling and a
noisy neighbour; report it, never the mean. Output is a per-blob table plus
a corpus-weighted total and a status tally. Two properties to hold:

- **ns/insn** is the headline number. Compare only against the same
  preset: `-O2`/`-O3` library builds, ASan builds are for leak-hunting, not
  for numbers.
- **`decode_err` and `unsupported` must be zero.** The corpus is pure
  integer base-ISA compiler output, so a non-zero count is a decode
  regression, not a workload property.

No google-benchmark: this is one timer and one loop, and adding a system
dependency to measure a function would be backwards.

## Layer 2b: the executor benchmark (`bench/c/exec_bench.c`)

`make bench-exec` runs `oemu-bench-exec`, which measures the whole pipeline --
fetch, decode, dispatch, execute -- through `oemu_exec_run()`. The guest is a
counted compute loop (an ALU/logic/`ldr`/`str` mix closed by a backwards
`b.ne`, ending in a real `exit(0)` SVC) emitted by bit-field encoders in the
source; each encoder was checked against `clang --target=aarch64-none-elf`,
so this benchmark needs neither the corpus nor a cross toolchain, and never
trusts a hand-written hex immediate. It self-verifies first: the guest must
retire the exact instruction count the emitted program implies, so a wrong
encoding fails loudly rather than reporting a flattering number.

Baseline (this dev machine, `PRESET=release`, `--count 60000 --trials 7`):

```
insns/run 840004   min ns/insn 30.4   throughput 32.9 Minsn/s
```

That lands within a hair of the decode benchmark's ~31 ns/insn, which is the
expected result for a dispatch-and-execute interpreter: the loop overhead is
the floor, and decode is only part of it. The debug preset is `# line
built with `-g` and no optimisation, so its number (roughly 90 ns/insn) is a
correctness signal, not a speed figure -- compare only against `release`.
Tune the workload with `--count N` (1..65535) and the timing with
`--trials`/`--budget`, exactly as for the decode benchmark.

## Layer 3: the guest phase (`bench/guest/`)

For when an ELF loader lands. The executor and the three-call SVC layer this
guest needs already exist and are exercised by `exec_bench`; what is still
missing is loading a file from disk. What is already here:

- `guest_syscalls.{h,c}` — the entire libc of this world: `write`,
  `clock_gettime`, `exit_group` as SVC stubs. That trio is exactly what the
  README's "SVC-based system-call entry" scope implies an oemu syscall layer
  must implement, so the guest and the emulator's requirements are pinned to
  each other.
- `crt0.c` — `_start` plus the freestanding `memcpy`/`memmove`/`memset` the
  compiler emits implicit calls to.
- `guest.ld` — two-`PT_LOAD` layout, text at `0x400000`, minimal enough that
  the future loader has almost nothing to implement.
- `guest_bench.c` — "intmix": deterministic, self-checking (redundant
  computation, no oracle file), sized for interpreted runs. The default
  binary's stdout is byte-identical run to run, so `oemu run` can eventually
  diff it against a golden trace; the `-timing` variant adds an `elapsed_ns`
  line for throughput.
- `build.sh` — probes linkers rather than assuming one (see below).
- `fetch-coremark.sh` — clones CoreMark for the real number once a libc
  answer exists.

### The toolchain finding (measured on the current dev machine, Ubuntu 24.04)

clang 18 compiles and assembles AArch64 flawlessly, **but linking fails
without one extra package**: the distro's `lld` and `gold` builds were
trimmed and report `unknown emulation: elf_aarch64` /
`unsupported ELF machine number 183`. One of

```sh
sudo apt-get install -y binutils-aarch64-linux-gnu   # cross GNU ld + as
sudo apt-get install -y mold                        # multi-arch drop-in
```

fixes it permanently; `build.sh` detects whichever is present and prints
that hint when neither is. Nothing else about the guest layer needs the
network at build time.

### Roadmap from here

1. ~~Execute loop + guest memory.~~ done (the executor and `oemu_memory`).
2. ~~Syscall layer: the three calls above.~~ done (`oemu_sysenv`; `build.sh`
   linking is the only thing still gated on the apt package above).
3. ELF loader for two-`PT_LOAD` static images (enough for everything here).
4. `make bench-e2e`: run `bench/guest/build/oemu-guest-bench` under oemu,
   diff stdout against the recorded golden line, report throughput from the
   `-timing` variant.
5. CoreMark on top, as the comparable headline number.

## Suites evaluated before building our own

| Candidate | Verdict |
| --- | --- |
| CoreMark (`github.com/eembc/coremark`) | best E2E candidate once layer 3 lands: pure integer, universally quoted; fetched, never vendored (GPL guest image) |
| Dhrystone (public domain) | vendor-worthy single file once an authoritative copy can be pinned; deliberately not hand-rewritten here — a wrong loop bound changes the unit |
| MiBench (SnuK) | good later corpus/E2E source (crc, string, matrix, sha); canonical upstream moves around |
| CPU Action | purpose-built for emulator benchmarking (coremark/dhrystone as static aarch64 ELFs); two of its four workloads need FP, which is out of scope, and the repo URL could not be verified from this environment |
| ARM ACT (official AArch64 compliance tests) | correctness, not throughput — worth borrowing per-instruction vectors against `oemu_decode` later |
| SPEC CPU | licence-blocked |
| LLVM test-suite / lnt | too heavy, FP-heavy, compiler-oriented |

A survey of ready-made suites is all very well; the decode corpus could not
be imported from any of them, because none ships *raw instruction streams
plus a decode-only contract* — hence layer 1 is ours, generated by a
compiler, exactly as this project consumes it.
