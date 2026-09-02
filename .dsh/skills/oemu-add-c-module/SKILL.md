---
name: oemu-add-c-module
description: Use when adding a new C module to oemu or making existing C code testable - deciding what belongs in a public header versus an internal one, keeping headers C++-compatible for the GoogleTest suite, routing allocation through the allocator seam, and choosing between status codes and fatal checks.
---

# Adding a C module to oemu

Production code is pure C11; only test translation units are C++17. Four
conventions make that combination work. They are the substance of this project's
test architecture — violating one breaks the build or silently removes code from
test coverage.

## Module layout

```
include/oemu/<module>.h          public API, C++-safe
src/<module>/<module>.c          implementation
src/<module>/<module>_internal.h internal interface, for tests (optional)
tests/unit/test_<module>.cpp     black-box tests
tests/unit/test_<module>_internal.cpp  white-box tests (optional)
```

Small modules with no internals can live in `src/core/` next to `status.c` and
`version.c`.

## 1. Public headers must be C++-safe

The C++ tests include these headers directly. Without `extern "C"` they emit
mangled symbol references and fail to link.

```c
#ifndef OEMU_MYMODULE_H
#define OEMU_MYMODULE_H

#include <stddef.h>

#include "oemu/macros.h"
#include "oemu/status.h"

OEMU_BEGIN_DECLS   /* expands to extern "C" { under C++ */

typedef struct oemu_thing {
  unsigned char *data;
  size_t len;
} oemu_thing;

OEMU_NODISCARD oemu_status oemu_thing_init(oemu_thing *thing, size_t cap);
void oemu_thing_dispose(oemu_thing *thing);

OEMU_END_DECLS

#endif /* OEMU_MYMODULE_H */
```

Keep out of public headers, because C++ cannot parse them: designated
initialisers, VLAs, `restrict`, `_Generic`, `_Atomic`, `typeof`. Use them freely
inside `.c` files.

`OEMU_NODISCARD` makes an ignored status a warning; `-Werror` then makes it an
error. Apply it to every function returning `oemu_status`.

## 2. Internal logic goes in `<module>_internal.h`, never `static`

A `static` function cannot be reached from a test. The two common escapes are
both worse than a private header: leaving logic untested, or having the test
`#include "mymodule.c"`, which recompiles C as C++ and breaks on the first
C-only construct.

Instead:

```c
/* src/mymodule/mymodule_internal.h -- NOT part of the public API. */
#ifndef OEMU_SRC_MYMODULE_INTERNAL_H
#define OEMU_SRC_MYMODULE_INTERNAL_H

#include "oemu/macros.h"
#include "oemu/status.h"

OEMU_BEGIN_DECLS

/* Pure function: testable exhaustively without allocating. */
OEMU_NODISCARD oemu_status oemu_mymodule_internal_pick_size(size_t current,
                                                            size_t required,
                                                            size_t *out);

OEMU_END_DECLS
#endif
```

Rules: never installed, never included by another module's public header,
symbols prefixed `oemu_<module>_internal_`, and changes here are not public API
breaks.

Wire it up in `src/CMakeLists.txt`:

```cmake
target_sources(oemu PRIVATE mymodule/mymodule_internal.h)   # for IDEs
target_include_directories(oemu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/mymodule)
```

`OEMU_INTERNAL_INCLUDE_DIR` (the `src` root) is already exported to the tests,
so the white-box test includes `"mymodule/mymodule_internal.h"` and needs no
extra CMake beyond the `INTERNAL` flag:

```cmake
oemu_add_test(test_mymodule_internal
  SOURCES unit/test_mymodule_internal.cpp
  INTERNAL
  LABELS "unit;whitebox"
)
```

Prefer extracting *pure* decision functions — size arithmetic, state
transitions, validation. They test exhaustively at boundary values with no
setup, which is where most real bugs hide.

## 3. All allocation goes through the allocator seam

Never call `malloc`/`realloc`/`free` directly in `src/`. gmock cannot intercept
C free functions — it needs virtual dispatch — so the library provides its own
indirection instead:

```c
#include "oemu/allocator.h"

const oemu_allocator *alloc = oemu_allocator_get();
void *p = alloc->alloc(size, alloc->user_data);
if (p == NULL) {
  return OEMU_ERR_NO_MEMORY;
}
...
alloc->free(p, alloc->user_data);
```

This is what makes out-of-memory branches reachable in tests. Bypassing it
leaves those paths permanently untested.

**Keep the object valid after a failed allocation.** `realloc` returning `NULL`
leaves the original block intact, so do not overwrite the pointer until it
succeeds:

```c
unsigned char *data = alloc->realloc(buf->data, new_cap, alloc->user_data);
if (data == NULL) {
  return OEMU_ERR_NO_MEMORY;   /* buf->data still valid */
}
buf->data = data;
buf->cap = new_cap;
```

Tests then assert that the object survived the failure — see
`BufferOom.BufferStaysUsableAfterAFailedAppend`.

**Check size arithmetic.** `len + additional` can wrap. Use a checked helper and
return `OEMU_ERR_OVERFLOW`; `tests/unit/test_buffer_internal.cpp` shows the
`SIZE_MAX` boundary cases worth covering.

## 4. Status codes for recoverable failures, `OEMU_REQUIRE` for bugs

Return `oemu_status` for anything a caller could handle: invalid arguments,
allocation failure, overflow, out-of-range. Those stay assertable in ordinary
tests.

```c
oemu_status oemu_thing_init(oemu_thing *thing, size_t cap) {
  if (thing == NULL) {
    return OEMU_ERR_INVALID_ARG;
  }
  ...
}
```

Use `OEMU_REQUIRE` (from `oemu/check.h`) only for programming errors that cannot
be reported. It aborts, and unlike `assert` it stays active in release builds, so
the contract cannot silently vanish from a shipped binary. Verify it with a death
test.

Add new codes to both `oemu_status` in `include/oemu/status.h` and the switch in
`src/core/status.c`. The switch has no `default` inside the enum range, so
`-Wswitch-enum` flags a forgotten case at compile time.

## Writing the tests

A fixture that installs `TrackingAllocator` gives every case a free leak check:

```cpp
class ThingTest : public ::testing::Test {
 protected:
  void SetUp() override { ASSERT_EQ(OEMU_OK, oemu_thing_init(&thing_, 0)); }
  void TearDown() override {
    oemu_thing_dispose(&thing_);
    EXPECT_EQ(0u, tracker_.live_blocks()) << "module leaked memory";
  }
  oemu_test::TrackingAllocator tracker_;
  oemu_thing thing_{};
};
```

Inject out-of-memory failures with `FailingAllocator`:

```cpp
TEST(ThingOom, InitReportsAllocationFailure) {
  oemu_test::FailingAllocator failing(1);   // 1-based: fail the first attempt
  oemu_thing thing{};
  EXPECT_EQ(OEMU_ERR_NO_MEMORY, oemu_thing_init(&thing, 32));
  oemu_thing_dispose(&thing);
}
```

Use `set_fail_on_call(n)` to fail a later allocation after some succeed, and
`kNever` to disable. Both doubles are RAII: they install on construction and
restore the previous allocator on destruction, so a failing test cannot leak its
override into the next one. Never install one without that scope guard.

Cover, at minimum: the happy path, every `NULL` argument, zero-size edge cases,
each error branch, and one "still usable after a failure" case.

Death tests belong in their own file, labelled `death`, with
`::testing::FLAGS_gtest_death_test_style = "threadsafe"` set in `SetUp` — the
default `fast` style can report spurious leaks from the aborted child under ASan.

**A function that terminates the process must flush gcov counters.** `abort()`
ends the process abnormally, so the coverage runtime never writes its data and
the function reads as 0% even when every death test exercises it. Follow
`src/core/check.c`: call `__gcov_dump()` immediately before `abort()`, guarded by
`#if defined(OEMU_COVERAGE)`. GCC defines no macro of its own for `--coverage`,
which is why `cmake/Coverage.cmake` defines that one.

## Registering the module

`src/CMakeLists.txt`:

```cmake
add_library(oemu
  ...
  mymodule/mymodule.c
)
```

`tests/CMakeLists.txt`: add the `oemu_add_test(...)` calls, then add the new
targets to `add_dependencies(check ...)`.

## Verify

```sh
make test    # expect 100% passed
make asan    # mandatory for allocation/lifetime/arithmetic changes
make clang   # Clang catches diagnostics GCC misses, e.g. -Wunreachable-code
```

Then check the new code is actually covered:

```sh
make coverage-summary
```

A new file showing low coverage usually means an error branch has no test, or a
default implementation was shadowed by a test double.
