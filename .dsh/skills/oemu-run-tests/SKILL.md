---
name: oemu-run-tests
description: Use when running, filtering, or debugging oemu's GoogleTest suite - selecting individual cases, using labels to skip slow death tests, interpreting failures, or investigating why a test binary crashes or hangs rather than failing cleanly.
---

# Running oemu tests

The suite is GoogleTest driven through CTest. `gtest_discover_tests` registers
every `TEST()` as its own CTest case, so failures name the exact case and
selection is precise.

Expected baseline: **84 cases, 100% passing**. Report actual counts; never
assume.

## Fast path

```sh
make test        # configure + build + run everything (debug)
make test-fast   # skip the slower death tests
make test-death  # only the death tests
make retest      # rerun only what failed last time
```

Underneath, that is plain CMake — use it directly when you need finer control:

```sh
cmake --preset debug
cmake --build build/debug
ctest --preset debug
```

## Selecting cases

```sh
ctest --test-dir build/debug -N                 # list every case without running
ctest --test-dir build/debug -R Buffer          # by name regex
ctest --test-dir build/debug -R 'BufferOom\.'   # one suite
ctest --test-dir build/debug -L whitebox        # by label
ctest --test-dir build/debug -LE death          # exclude a label
ctest --test-dir build/debug --rerun-failed
ctest --test-dir build/debug --output-on-failure -j"$(nproc)"
```

Labels in use: `unit` on everything, plus `whitebox` (tests reaching internal
headers) and `death` (fork-based abort tests).

### Labels are one composite string

`oemu_add_test(... LABELS unit death)` produces the single label `unit.death`,
not two labels. This is deliberate and must not be "fixed" into a list.

CMake flattens the `PROPERTIES` argument of `gtest_discover_tests` into a
semicolon-separated list, so a genuine multi-value `LABELS` property cannot
survive: the extra values are misread as further property names and silently
dropped, leaving only the first label. No escaping avoids this. Because
`ctest -L` matches by **regex**, joining components with `.` keeps `-L unit`,
`-L death` and `-LE death` all working.

If you add a label and `-L <label>` returns 0 cases, inspect what was actually
generated:

```sh
ctest --test-dir build/debug -N >/dev/null          # PRE_TEST discovery writes the files
grep -o 'LABELS[^)]*' build/debug/tests/*_tests.cmake | sort -u
```

## Running a test binary directly

For gtest's own flags, bypass CTest:

```sh
./build/debug/bin/test_buffer --gtest_filter='BufferOom.*'
./build/debug/bin/test_buffer --gtest_brief=1          # only failures
./build/debug/bin/test_buffer --gtest_repeat=100 --gtest_shuffle
./build/debug/bin/test_buffer --gtest_list_tests
./build/debug/bin/test_buffer --gtest_break_on_failure # for a debugger
```

Test binaries live in `build/<preset>/bin/`.

## Debugging failures

**Read the assertion first.** GoogleTest prints the expected and actual values;
`EXPECT_EQ(OEMU_OK, status)` shows both status codes. Use
`oemu_status_str(status)` when a numeric code is unclear.

**A leak reported at teardown, not in the test body.** `BufferTest::TearDown`
asserts `tracker_.live_blocks() == 0`. A failure there means the module leaked:
look for an early `return` that skips `oemu_buffer_dispose`, or a realloc whose
failure path dropped the original pointer.

**Test passes alone but fails in the suite.** Almost always allocator state.
`TrackingAllocator` and `FailingAllocator` install themselves on construction
and restore on destruction; a test that installs one without RAII leaks the
override into the next test. Confirm with:

```sh
./build/debug/bin/test_buffer --gtest_filter='TheOneTest.*'   # isolated
ctest --test-dir build/debug -R TheOneTest                    # in context
```

**Crash instead of a clean failure.** Run it under ASan — it locates the fault
precisely:

```sh
make asan
./build/asan/bin/test_buffer --gtest_filter='Failing.*'
```

**A death test fails or reports leaks.** Death tests fork; under ASan the
default `fast` style can report leaks from the aborted child.
`tests/unit/test_check.cpp` sets `threadsafe` style in `SetUp` for that reason.
Keep death tests in their own file and label them `death`.

**A case times out.** Each case has a 60-second CTest timeout. A death test
that hangs usually means the code under test did not actually abort — verify
the `OEMU_REQUIRE` condition is genuinely false.

## When adding tests

Register the file in `tests/CMakeLists.txt`:

```cmake
oemu_add_test(test_mymodule
  SOURCES unit/test_mymodule.cpp
  LABELS unit
)
```

Add `INTERNAL` to reach `src/<module>/<module>_internal.h`. Also add the target
to the `check` custom target's `add_dependencies` list in the same file.

For the techniques themselves — allocator seams, OOM injection, white-box
tests, death tests — load the `oemu-add-c-module` skill.
