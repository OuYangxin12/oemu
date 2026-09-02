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

# Death tests are separated out: they fork/re-exec and are slower, so a
# developer can skip them with `ctest -LE death`.
oemu_add_test(test_check
  SOURCES unit/test_check.cpp
  LABELS "unit;death"
)

# Reaches into src/buffer/buffer_internal.h to test the growth policy directly.
oemu_add_test(test_buffer_internal
  SOURCES unit/test_buffer_internal.cpp
  INTERNAL
  LABELS "unit;whitebox"
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
