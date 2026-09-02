# AGENTS.md

Project instructions for coding agents working on **oemu**, a pure C11 project
with a GoogleTest-based local test harness.

This file is the baseline context. Task-specific procedures live in
`.dsh/skills/`; load the relevant skill before acting.

| Skill | Load it when |
| --- | --- |
| `oemu-run-tests` | running, filtering or debugging tests |
| `oemu-build-configs` | building, or using sanitizers and coverage |
| `oemu-add-c-module` | adding or changing a C module |
| `oemu-ci-workflow` | touching CI, or reproducing a CI failure locally |

## What this project is

- Production code is **C11**, compiled with hidden visibility and no C++ dependency.
- **Only test translation units are C++17**, linked against the C library through `extern "C"`.
- Build system is CMake ≥ 3.20 with presets; Ninja is the generator.
- Tests use GoogleTest from the **system package** (`find_package(GTest)`), not vendored.

## Non-negotiable rules

1. **Never add a C++ dependency to `src/` or `include/`.** Production code stays pure C11. C++ belongs in `tests/` only.

2. **Every public header wraps declarations in `OEMU_BEGIN_DECLS` / `OEMU_END_DECLS`.** Without those `extern "C"` guards the C++ tests fail to link. Keep designated initialisers, VLAs, `restrict` and `_Generic` out of public headers — C++ cannot parse them.

3. **Do not make logic `static` when it needs testing.** Expose it through `src/<module>/<module>_internal.h`. Never write a test that `#include`s a `.c` file.

4. **All allocation goes through `oemu_allocator`.** Never call `malloc`/`free` directly in `src/`. The indirection is what makes out-of-memory paths testable.

5. **Recoverable failures return `oemu_status`.** Reserve `OEMU_REQUIRE` (abort) for programming errors that cannot be reported to a caller.

6. **The build must stay warning-free.** `debug`, `release` and `clang` presets compile with `-Werror`.

## Verification expectation

Before claiming a change works, run at least:

```sh
make test        # debug build, full suite
make asan        # ASan + UBSan
```

Both must report `100% tests passed`. For anything touching allocation,
lifetime, or arithmetic, `make asan` is mandatory rather than optional.

Do not claim a configuration passes without having run it. Report the actual
counts.

## Environment notes

Two environment problems are already solved in the build files; do not
"fix" them again differently.

- **conda shadows the system GoogleTest.** `CONDA_PREFIX` is added to `CMAKE_IGNORE_PREFIX_PATH` in the top-level `CMakeLists.txt`. Configure output names the selected GoogleTest; it must be the one under `/usr/lib`.
- **TSan aborts under Linux 6.x ASLR.** `cmake/Sanitizers.cmake` routes test binaries through `setarch -R` via `CROSSCOMPILING_EMULATOR`.

`lcov` is **not installed** in this environment, so `make coverage` (HTML) fails;
`make coverage-summary` works with plain gcov. `clang-format` and `clang-tidy`
are available, so `make format-check` and `make tidy` must both be clean before
you push — CI's `lint` job runs exactly those. Do not report a result you could
not actually produce.

## Style

- 2-space indent, 96-column limit, braces always (see `.clang-format`).
- Names are `lower_snake_case`; macros and enum constants are `UPPER_CASE`; public symbols are prefixed `oemu_`.
- Comment *why*, not *what*. Explain non-obvious constraints and the reason a workaround exists.
