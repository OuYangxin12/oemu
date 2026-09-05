# GoogleTest-based test harness.
#
# Uses the system GoogleTest package (find_package). If your environment lacks
# it, see the commented FetchContent fallback at the bottom of this file.

find_package(GTest REQUIRED)

# GoogleTest's death tests and its internals need threads.
find_package(Threads REQUIRED)

# Surface which GoogleTest actually got picked up: a stale copy from an active
# conda/homebrew prefix is the usual cause of confusing link errors.
if(GTest_DIR)
  message(STATUS "oemu: GoogleTest ${GTest_VERSION} from ${GTest_DIR}")
else()
  message(STATUS "oemu: GoogleTest ${GTest_VERSION}")
endif()

include(GoogleTest)

# --- shared test support library ---------------------------------------------

add_library(oemu_test_support STATIC
  support/tracking_allocator.cpp
)
target_include_directories(oemu_test_support PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}
)
target_link_libraries(oemu_test_support PUBLIC
  oemu
  GTest::gtest
  Threads::Threads
)
# Test support code is not held to the production warning set (-Wold-style-cast
# style flags interact badly with gtest macros), but keep the basics.
target_compile_options(oemu_test_support PRIVATE -Wall -Wextra)

# --- helper: declare one test executable -------------------------------------
#
# oemu_add_test(<name> SOURCES <files...> [INTERNAL] [LABELS <labels...>])
#
#   INTERNAL  grants access to src/<module>/<module>_internal.h headers.
#   LABELS    label components joined into one composite ctest label.
#
# On labels: CMake flattens gtest_discover_tests' PROPERTIES argument into a
# semicolon-separated list, so a genuine multi-value LABELS property cannot pass
# through -- the extra values would be misread as further property names. Since
# `ctest -L` matches labels by regular expression, the components are joined into
# a single label instead ("unit.death"), which keeps `-L unit`, `-L death` and
# `-LE death` all working as expected.
#
function(oemu_add_test name)
  cmake_parse_arguments(ARG "INTERNAL" "" "SOURCES;LABELS" ${ARGN})

  if(NOT ARG_SOURCES)
    message(FATAL_ERROR "oemu_add_test(${name}): SOURCES is required")
  endif()

  add_executable(${name} ${ARG_SOURCES})

  target_link_libraries(${name} PRIVATE
    oemu
    oemu_test_support
    GTest::gtest
    GTest::gtest_main
    GTest::gmock
    Threads::Threads
  )

  if(ARG_INTERNAL)
    # White-box tests reach internal headers via the src root, so the include
    # reads `#include "buffer/buffer_internal.h"` and stays unambiguous.
    target_include_directories(${name} PRIVATE ${OEMU_INTERNAL_INCLUDE_DIR})
  endif()

  target_compile_options(${name} PRIVATE -Wall -Wextra)

  # Sanitizer / coverage flags must reach the test binaries too.
  target_link_libraries(${name} PRIVATE oemu_compiler_flags)

  # Join the label components with "." into one composite label (see above).
  if(ARG_LABELS)
    string(REPLACE ";" "." composite_label "${ARG_LABELS}")
  else()
    set(composite_label "unit")
  endif()

  # Under TSan the binaries must run through `setarch -R`; see Sanitizers.cmake.
  # CROSSCOMPILING_EMULATOR is what gtest_discover_tests uses to launch them,
  # so it also applies during test discovery, not just at run time.
  if(OEMU_TEST_LAUNCHER)
    set_target_properties(${name} PROPERTIES
      CROSSCOMPILING_EMULATOR "${OEMU_TEST_LAUNCHER}")
  endif()

  # Registers each TEST() as an individual ctest case, so failures name the
  # exact case and `ctest -R` can select one.
  gtest_discover_tests(${name}
    DISCOVERY_MODE PRE_TEST
    PROPERTIES
      LABELS ${composite_label}
      TIMEOUT 60
  )
endfunction()

# --- test targets -------------------------------------------------------------

oemu_add_test(test_status
  SOURCES unit/test_status.cpp
  LABELS unit
)

oemu_add_test(test_version
  SOURCES unit/test_version.cpp
  LABELS unit
)

oemu_add_test(test_allocator
  SOURCES unit/test_allocator.cpp
  LABELS unit
)

oemu_add_test(test_buffer
  SOURCES unit/test_buffer.cpp
  LABELS unit
)

oemu_add_test(test_regs
  SOURCES unit/test_regs.cpp
  LABELS unit
)

oemu_add_test(test_decode
  SOURCES unit/test_decode.cpp
  LABELS unit
)

# Death tests are separated out: they fork/re-exec and are slower, so a
# developer can skip them with `ctest -LE death`.
oemu_add_test(test_check
  SOURCES unit/test_check.cpp
  LABELS "unit;death"
)

# Also death tests for the regs module.
oemu_add_test(test_regs_check
  SOURCES unit/test_regs_check.cpp
  LABELS "unit;death"
)

# Reaches into src/buffer/buffer_internal.h to test the growth policy directly.
oemu_add_test(test_buffer_internal
  SOURCES unit/test_buffer_internal.cpp
  INTERNAL
  LABELS "unit;whitebox"
)

# Reaches into src/regs/regs_internal.h: exhaustive condition-code table and the
# carry/overflow derivation.
oemu_add_test(test_regs_internal
  SOURCES unit/test_regs_internal.cpp
  INTERNAL
  LABELS "unit;whitebox"
)

# Reaches into src/decode/decode_internal.h: sign-extension at every encoded
# width, and DecodeBitMasks swept across its whole input space.
oemu_add_test(test_decode_internal
  SOURCES unit/test_decode_internal.cpp
  INTERNAL
  LABELS "unit;whitebox"
)

# The region-table memory model, including the OOM paths the allocator seam
# exposes. Reaches into src/memory/memory_internal.h for the containment and
# overlap arithmetic, so it counts as white-box.
oemu_add_test(test_memory
  SOURCES unit/test_memory.cpp
  INTERNAL
  LABELS "unit;whitebox"
)

# The system-mode bus: RAM regions, MMIO device regions and the fake device
# double. Reaches into src/aspace/aspace_internal.h for the region-table
# helpers and byte assembly, so it counts as white-box.
oemu_add_test(test_aspace
  SOURCES unit/test_aspace.cpp
  INTERNAL
  LABELS "unit;whitebox"
)

# The machine: its two allocations, their failure paths, and the sticky
# lifecycle events.
oemu_add_test(test_machine
  SOURCES unit/test_machine.cpp
  LABELS unit
)

# The four-syscall guest ABI: write capture, clock layout, exit, errno fidelity.
oemu_add_test(test_sysenv
  SOURCES unit/test_sysenv.cpp
  LABELS unit
)

# End-to-end interpreter: small hand-assembled programs driven through
# oemu_exec_step / oemu_exec_run.
oemu_add_test(test_exec
  SOURCES unit/test_exec.cpp
  LABELS unit
)

# The bus seam: the memory and address-space views, the environment view, and
# the executor driven over an oemu_aspace with an MMIO device. Reaches into
# src/exec/exec_internal.h for the seam-level dispatch, so it is white-box.
oemu_add_test(test_memops
  SOURCES unit/test_memops.cpp
  INTERNAL
  LABELS "unit;whitebox"
)

# Reaches into src/exec/exec_internal.h: the shifted/extended operand edges,
# the 128-bit-free multiply, one-source helpers, and single-opcode dispatch.
oemu_add_test(test_exec_internal
  SOURCES unit/test_exec_internal.cpp
  INTERNAL
  LABELS "unit;whitebox"
)

# Black-box ELF loader tests: images are assembled byte-by-byte in the test,
# covering the whole status contract and a load-then-run integration case.
oemu_add_test(test_elf
  SOURCES unit/test_elf.cpp
  LABELS unit
)

# Reaches into src/elf/elf_internal.h: little-endian readers at odd offsets,
# bounds/overflow edges, the segment validator and range-overlap.
oemu_add_test(test_elf_internal
  SOURCES unit/test_elf_internal.cpp
  INTERNAL
  LABELS "unit;whitebox"
)

# Black-box tests for the `oemu run` CLI: fork/exec the real binary against
# byte-built ELF files and check the exit-code contract. Needs the oemu binary
# built first, so it depends on that target (the target is `oemu_cli`; its
# OUTPUT_NAME is `oemu`).
oemu_add_test(test_cli
  SOURCES unit/test_cli.cpp
  LABELS unit
)
add_dependencies(test_cli oemu_cli)

# System-register state: boot PSTATE, the identification block and the
# table-driven MRS/MSR contract, including the two Undefined signals.
oemu_add_test(test_sysreg
  SOURCES unit/test_sysreg.cpp
  LABELS unit
)

# Reaches into src/sysreg/sysreg_internal.h: table-wide row well-formedness,
# assembler-harvested encoding pins, and init's reset sweep.
oemu_add_test(test_sysreg_internal
  SOURCES unit/test_sysreg_internal.cpp
  INTERNAL
  LABELS "unit;whitebox"
)

# Death tests for the sysreg module's fatal contracts (EL2 boot, NULL state).
oemu_add_test(test_sysreg_check
  SOURCES unit/test_sysreg_check.cpp
  LABELS "unit;death"
)

# --- convenience targets ------------------------------------------------------

# `cmake --build build --target check` builds everything then runs ctest.
add_custom_target(check
  COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure
  WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
  USES_TERMINAL
)
add_dependencies(check
  test_status
  test_version
  test_allocator
  test_buffer
  test_check
  test_buffer_internal
  test_regs
  test_regs_check
  test_regs_internal
  test_decode
  test_decode_internal
  test_memory
  test_aspace
  test_machine
  test_sysenv
  test_memops
  test_exec
  test_exec_internal
  test_elf
  test_elf_internal
  test_cli
  test_sysreg
  test_sysreg_internal
  test_sysreg_check
)

# Registers the `coverage` target now that the test targets exist.
oemu_add_coverage_target()

# --- FetchContent fallback (reference) ---------------------------------------
#
# Replace the find_package(GTest REQUIRED) call above with the following to
# build a pinned GoogleTest from source instead of using the system package:
#
#   include(FetchContent)
#   FetchContent_Declare(googletest
#     GIT_REPOSITORY https://github.com/google/googletest.git
#     GIT_TAG        v1.15.2
#     GIT_SHALLOW    TRUE
#   )
#   set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
#   set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
#   FetchContent_MakeAvailable(googletest)
#   add_library(GTest::gtest ALIAS gtest)          # if the aliases are missing
#   add_library(GTest::gtest_main ALIAS gtest_main)
#   add_library(GTest::gmock ALIAS gmock)
