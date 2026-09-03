cmake_minimum_required(VERSION 3.20)

# The C++ language is enabled unconditionally because GoogleTest requires it.
# Production code stays pure C11; only test translation units are C++.
project(oemu
  VERSION 0.1.0
  DESCRIPTION "Pure C project skeleton with a GoogleTest-based local test harness"
  LANGUAGES C CXX
)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Always emit compile_commands.json so clangd / clang-tidy work out of the box.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Keep binaries in predictable locations regardless of generator.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

# Only the top-level project drives testing, warnings-as-errors, etc.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(OEMU_IS_TOP_LEVEL ON)
else()
  set(OEMU_IS_TOP_LEVEL OFF)
endif()

option(OEMU_BUILD_TESTS "Build the GoogleTest test suite" ${OEMU_IS_TOP_LEVEL})
option(OEMU_BUILD_BENCH "Build the decode-throughput microbenchmarks" ${OEMU_IS_TOP_LEVEL})
option(OEMU_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(OEMU_ENABLE_COVERAGE "Instrument the build for gcov/lcov coverage" OFF)
option(OEMU_IGNORE_CONDA_PREFIX
  "Ignore an active conda prefix when searching for dependencies such as GoogleTest" ON)
set(OEMU_SANITIZERS "" CACHE STRING
  "Semicolon-separated sanitizers to enable: address;undefined;thread;leak")

# An active conda environment sits on CMAKE_PREFIX_PATH and routinely shadows
# system packages with older builds (e.g. GoogleTest 1.11 instead of 1.14),
# which then link against a different C++ runtime than the compiler in use.
# Skipping that prefix keeps dependency resolution predictable.
if(OEMU_IGNORE_CONDA_PREFIX AND DEFINED ENV{CONDA_PREFIX})
  if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.23)
    list(APPEND CMAKE_IGNORE_PREFIX_PATH "$ENV{CONDA_PREFIX}")
    message(STATUS "oemu: ignoring conda prefix $ENV{CONDA_PREFIX} during package discovery")
  else()
    message(WARNING
      "oemu: OEMU_IGNORE_CONDA_PREFIX needs CMake 3.23+; "
      "dependencies may resolve to $ENV{CONDA_PREFIX}")
  endif()
endif()

include(CompilerWarnings)
include(Sanitizers)
include(Coverage)

# oemu_compiler_flags carries warnings, sanitizer and coverage settings.
# Every target in the project links it so the configuration stays in one place.
add_library(oemu_compiler_flags INTERFACE)
oemu_set_project_warnings(oemu_compiler_flags)
oemu_enable_sanitizers(oemu_compiler_flags)
oemu_enable_coverage(oemu_compiler_flags)

add_subdirectory(src)

if(OEMU_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(OEMU_BUILD_BENCH)
  add_subdirectory(bench)
endif()

message(STATUS "")
message(STATUS "oemu ${PROJECT_VERSION} configuration summary")
message(STATUS "  build type ......... ${CMAKE_BUILD_TYPE}")
message(STATUS "  C compiler ......... ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
message(STATUS "  tests .............. ${OEMU_BUILD_TESTS}")
message(STATUS "  bench .............. ${OEMU_BUILD_BENCH}")
message(STATUS "  sanitizers ......... ${OEMU_SANITIZERS}")
message(STATUS "  coverage ........... ${OEMU_ENABLE_COVERAGE}")
message(STATUS "  warnings as errors . ${OEMU_WARNINGS_AS_ERRORS}")
message(STATUS "")
