---
name: oemu-build-configs
description: Use when building oemu, choosing a build preset, running sanitizers (ASan/UBSan/TSan), generating coverage reports, or diagnosing configure-time failures such as the wrong GoogleTest being found or ThreadSanitizer aborting at startup.
---

# oemu build configurations

CMake ≥ 3.20 with presets, Ninja generator. Each preset builds into its own
tree under `build/<preset>/`, so configurations coexist — switching does not
force a rebuild.

## Presets

| Preset | Build type | Notes |
| --- | --- | --- |
| `debug` | Debug | `-Werror`; the default |
| `release` | RelWithDebInfo | `-Werror`; tests still enabled |
| `asan` | Debug | AddressSanitizer + UndefinedBehaviorSanitizer |
| `tsan` | Debug | ThreadSanitizer; auto-wraps tests in `setarch -R` |
| `coverage` | Debug | gcov instrumentation, `-O0 -fno-inline` |
| `clang` | Debug | forces the Clang toolchain |

```sh
make                    # debug: configure, build, test
make asan               # ASan + UBSan
make tsan
make release
make clang
make coverage-summary   # text coverage (gcov only)
make coverage           # HTML report (needs lcov)
make help               # every target
make distclean          # remove all build trees
```

Or directly:

```sh
cmake --preset asan
cmake --build build/asan --parallel
ctest --preset asan
```

Use `make <target> PRESET=<preset>` to point a target at another preset.

## Verification expectation

All five test-running presets must report `100% tests passed` (84 cases).
Coverage is ~93%.

Run `make test` **and** `make asan` before claiming a change works. For
allocation, lifetime, or arithmetic changes, ASan is mandatory. Run `make clang`
too when you touched warning-sensitive code: Clang catches diagnostics GCC
misses, notably `-Wunreachable-code`.

## Where the configuration lives

| File | Owns |
| --- | --- |
| `CMakeLists.txt` | options, C/C++ standards, conda workaround |
| `cmake/CompilerWarnings.cmake` | the warning set |
| `cmake/Sanitizers.cmake` | sanitizer flags, TSan ASLR workaround |
| `cmake/Coverage.cmake` | gcov instrumentation, report targets |
| `cmake/GcovSummary.cmake` | text summary, no lcov needed |
| `src/CMakeLists.txt` | library targets, install rules |
| `tests/CMakeLists.txt` | `oemu_add_test`, GoogleTest lookup |

Options: `OEMU_BUILD_TESTS`, `OEMU_WARNINGS_AS_ERRORS`, `OEMU_ENABLE_COVERAGE`,
`OEMU_SANITIZERS`, `OEMU_IGNORE_CONDA_PREFIX`.

All flags flow through one INTERFACE target, `oemu_compiler_flags`. Add shared
flags there, not per-target.

## Warnings

`-Werror` is on for `debug`, `release` and `clang`. Fix warnings; do not
suppress them or configure with `-DOEMU_WARNINGS_AS_ERRORS=OFF` to get a build
through.

C-only flags (`-Wstrict-prototypes`, `-Wmissing-prototypes`,
`-Wjump-misses-init`, ...) are wrapped in `$<COMPILE_LANGUAGE:C>`. Without that
guard GCC emits "valid for C but not for C++" on every C++ test file. When
adding a warning flag, decide which list it belongs in:

```cmake
set(gcc_clang_common -Wall -Wextra ...)   # both languages
set(c_only -Wstrict-prototypes ...)       # C only
```

## Sanitizers

```sh
cmake --preset asan          # OEMU_SANITIZERS="address;undefined"
cmake -DOEMU_SANITIZERS="address" -S . -B build/custom   # ad hoc
```

Accepted values: `address`, `undefined`, `thread`, `leak`, `memory` (aliases
`asan`, `ubsan`, `tsan`, `lsan`, `msan`). Flags are applied for both compiling
and linking, since sanitizers need their runtime linked in.

ASan and TSan are mutually exclusive; the CMake code rejects the combination at
configure time rather than letting it fail at runtime.

UBSan runs with `-fno-sanitize-recover=all`, so undefined behaviour fails the
run instead of only printing a report.

Useful runtime options:

```sh
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 ./build/asan/bin/test_buffer
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 ./build/asan/bin/test_buffer
```

### TSan aborts at startup

```
FATAL: ThreadSanitizer: unexpected memory mapping 0x7...
```

Linux 6.x — including WSL2 — uses more ASLR entropy than TSan's shadow-memory
layout expects. `cmake/Sanitizers.cmake` detects a TSan build and sets the
`CROSSCOMPILING_EMULATOR` target property to `setarch -R`.

`CROSSCOMPILING_EMULATOR` specifically is what makes this work:
`gtest_discover_tests` executes each binary at **discovery** time, not only
during the run, so a launcher applied to test cases alone would still fail
while generating the case list.

Alternative if `setarch` is unavailable: `sudo sysctl vm.mmap_rnd_bits=28`.

## Coverage

```sh
make coverage-summary   # per-file text summary; needs only gcov
make coverage           # HTML in build/coverage/coverage-html; needs lcov
```

`lcov` is not installed in this environment, so `make coverage` fails with an
explicit install hint. Use `coverage-summary` and report those numbers instead
of claiming an HTML report exists.

`cmake/GcovSummary.cmake` runs in CMake script mode, invokes `gcov` per `.gcda`,
and filters to `src/` and `include/` so system headers, GoogleTest and the test
files do not dilute the result.

Coverage builds force `-O0 -fno-inline` for accurate line mapping, so those
numbers say nothing about optimised behaviour.

Low coverage in a file whose logic is exercised indirectly usually means a
default implementation was replaced by a test double — `src/core/allocator.c`
needed a test calling the real `malloc` path explicitly.

## Configure-time problems

### The wrong GoogleTest is found

Symptom: configure reports a version other than the system one, or C++ link
errors appear with correct-looking code.

An active conda environment puts `CONDA_PREFIX` on `CMAKE_PREFIX_PATH`, where it
shadows the system package — here conda's 1.11 instead of the system 1.14,
potentially built against a different C++ runtime.

`CMakeLists.txt` adds that prefix to `CMAKE_IGNORE_PREFIX_PATH` (needs CMake
3.23+). Verify:

```sh
cmake --preset debug 2>&1 | grep -i gtest
# -- oemu: GoogleTest 1.14.0 from /usr/lib/x86_64-linux-gnu/cmake/GTest
```

The path must be under `/usr/lib`. Opt out with
`-DOEMU_IGNORE_CONDA_PREFIX=OFF`.

### GoogleTest is missing entirely

```sh
sudo apt install libgtest-dev libgmock-dev
```

To pin a version instead, replace `find_package(GTest REQUIRED)` in
`tests/CMakeLists.txt` with the `FetchContent` block documented at the bottom of
that file.

### Stale cache after editing CMake files

Presets reconfigure automatically, but a changed generator, toolchain, or
`find_package` result can persist. Delete the tree:

```sh
rm -rf build/debug && make test
```

## Code quality targets

```sh
make format         # apply clang-format
make format-check   # fail on deviation
make tidy           # clang-tidy over C sources
make compile-db     # symlink compile_commands.json for clangd
```

`clang-format` and `clang-tidy` are available; both must be clean before a push,
since CI's `lint` job runs exactly these commands.

Two things about this setup are load-bearing:

- `.clang-format` must stay a **single** YAML document. A second document repeating `Language: Cpp` makes clang-format reject the whole file with `Error reading .clang-format: Invalid argument` — every file then "fails" formatting for a reason that has nothing to do with the code.
- `make tidy` passes `--extra-arg=-Wno-unknown-warning-option`. `compile_commands.json` is generated by GCC and carries GCC-only flags (`-Wlogical-op`, `-Wduplicated-cond`, `-Wjump-misses-init`, ...); Clang reports each as an unknown-warning-option **error** unrelated to the analysed code.
