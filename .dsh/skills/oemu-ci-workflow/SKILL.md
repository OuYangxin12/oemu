---
name: oemu-ci-workflow
description: Use when changing oemu's GitHub Actions workflow, adding a job or build matrix entry, or reproducing a CI failure locally - including failures that appear only in CI because the runner has tools or a toolchain the local environment lacks.
---

# oemu CI workflow

`.github/workflows/ci.yml`, on `ubuntu-24.04`. The guiding principle: **CI runs
the same commands a developer runs locally** — `cmake --preset X` and
`ctest --preset X` — so any failure is reproducible without pushing.

## Jobs

| Job | What it does |
| --- | --- |
| `test` | matrix over `debug` (gcc), `clang`, `release`, `asan`; each configures, builds, runs ctest |
| `coverage` | `coverage` preset, builds the `coverage` target, uploads HTML as an artifact |
| `lint` | `make format-check`, then `clang-tidy` over `src/*.c` |

`fail-fast: false` on the matrix, so one failing compiler still reports the
others. Concurrency is grouped per ref with `cancel-in-progress`.

## Reproducing a failure locally

Run the identical preset:

```sh
cmake --preset asan
cmake --build build/asan --parallel
ctest --preset asan --parallel 2
```

or the shorthand: `make asan`, `make clang`, `make release`, `make test`.

Sanitizer jobs export runtime options; match them when a failure appears only in
CI:

```sh
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1:strict_string_checks=1 \
UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
  ctest --preset asan
```

## Failures that only happen in CI

**The `lint` job fails.** Reproduce it exactly:

```sh
make format-check
make tidy
```

Both are runnable locally, so a lint failure should never reach CI. Two
config-level traps produce failures that look like code problems but are not:

- `Error reading .clang-format: Invalid argument` on *every* file means the config itself is unparseable, not that the code is misformatted. Keep `.clang-format` a single YAML document; a second document repeating `Language: Cpp` breaks it.
- clang-tidy reporting `unknown warning option '-Wlogical-op'` as an **error** comes from the GCC-generated `compile_commands.json`. `make tidy` already passes `--extra-arg=-Wno-unknown-warning-option`; invoking `clang-tidy` by hand without it reproduces the noise.

Formatting rules live in `.clang-format`, checks in `.clang-tidy`.

**The `coverage` job fails but `make coverage-summary` works.** `make coverage`
needs `lcov`, which is absent locally; the runner installs it.
`make coverage-summary` uses plain gcov and covers most needs, but it does not
validate the lcov invocation itself. When editing `cmake/Coverage.cmake`, note
that the lcov commands pass `--ignore-errors mismatch,unused,empty,gcov,source,negative`
to tolerate gcc/lcov version skew — the runner's pairing differs from a typical
local one.

**Clang fails, GCC passes.** Expected: Clang diagnoses things GCC does not, such
as `-Wunreachable-code` on a provably dead branch. Run `make clang` before
pushing. Both compile with `-Werror`.

**GoogleTest resolves differently.** Locally, an active conda prefix shadows the
system package; the CI runner has no conda. That is why the workaround is opt-out
(`OEMU_IGNORE_CONDA_PREFIX`) rather than unconditional. If a version-specific
issue appears, check which GoogleTest each side selected:

```sh
cmake --preset debug 2>&1 | grep -i gtest
```

**TSan is absent from CI on purpose.** It needs the `setarch -R` ASLR workaround
(see the `oemu-build-configs` skill) and adds little over ASan for this
single-threaded code. Run `make tsan` locally when adding concurrency; add a
matrix entry only if the project genuinely becomes threaded.

## Editing the workflow

Adding a matrix entry needs a preset in `CMakePresets.json` plus a row in
`ci.yml`:

```yaml
- name: my-config
  preset: my-config
```

Keep every step a preset invocation. Do not inline bare `cmake -D...` flags: it
splits local and CI behaviour, which is exactly what this setup avoids.

New tool dependencies go in that job's `apt-get install` list. Current
dependencies: `cmake`, `ninja-build`, `libgtest-dev`, `libgmock-dev`, plus
`clang` (matrix), `lcov` (coverage), `clang-format`/`clang-tidy` (lint).

Validate YAML before pushing, and confirm the preset works locally:

```sh
python3 -c 'import yaml,sys; yaml.safe_load(open(".github/workflows/ci.yml"))'
ctest --preset my-config
```

## Notes

- `ubuntu-24.04` ships GCC 13 and GoogleTest 1.14. Both are what the project targets; bumping the runner image can change either.
- Test discovery uses `DISCOVERY_MODE PRE_TEST`, so cases are enumerated at ctest time, not build time. A binary that cannot start makes discovery itself fail — the error names the executable rather than a test case.
- `actions/checkout@v4` and `actions/upload-artifact@v4` are pinned by major version.
