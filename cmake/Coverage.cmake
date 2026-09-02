# gcov-based coverage instrumentation plus an optional lcov report target.

function(oemu_enable_coverage target)
  if(NOT OEMU_ENABLE_COVERAGE)
    return()
  endif()

  if(MSVC)
    message(WARNING "oemu: coverage instrumentation is not supported with MSVC")
    return()
  endif()

  # --coverage implies -fprofile-arcs -ftest-coverage and links gcov.
  # Optimisation is disabled so line mapping stays accurate.
  target_compile_options(${target} INTERFACE --coverage -O0 -g -fno-inline)
  target_link_options(${target} INTERFACE --coverage)

  message(STATUS "oemu: coverage instrumentation enabled")
endfunction()

# Registers the `coverage` target. Called once from tests/CMakeLists.txt after
# the test targets exist, because the report depends on running them.
function(oemu_add_coverage_target)
  if(NOT OEMU_ENABLE_COVERAGE)
    return()
  endif()

  find_program(OEMU_LCOV lcov)
  find_program(OEMU_GENHTML genhtml)
  find_program(OEMU_GCOV gcov)

  # Text summary straight from gcov: always available when the compiler is, so
  # coverage numbers are reachable even without lcov installed.
  if(OEMU_GCOV)
    add_custom_target(coverage-summary
      COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure
      COMMAND "${CMAKE_COMMAND}"
        -DGCOV_EXECUTABLE=${OEMU_GCOV}
        -DBINARY_DIR=${CMAKE_BINARY_DIR}
        -DSOURCE_DIR=${CMAKE_SOURCE_DIR}
        -P "${CMAKE_SOURCE_DIR}/cmake/GcovSummary.cmake"
      WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
      COMMENT "Running tests and printing a per-file gcov summary"
      USES_TERMINAL
      VERBATIM
    )
  endif()

  if(NOT OEMU_LCOV OR NOT OEMU_GENHTML)
    # Note: keep the message free of shell metacharacters such as parentheses --
    # cmake -E echo arguments still pass through the shell under some generators.
    add_custom_target(coverage
      COMMAND "${CMAKE_COMMAND}" -E echo
        "ERROR: lcov and genhtml are required for the HTML coverage report."
      COMMAND "${CMAKE_COMMAND}" -E echo
        "Install them with: sudo apt-get install lcov"
      COMMAND "${CMAKE_COMMAND}" -E echo
        "Alternative without lcov: cmake --build <dir> --target coverage-summary"
      COMMAND "${CMAKE_COMMAND}" -E false
      COMMENT "HTML coverage report unavailable: lcov not found"
      VERBATIM
    )
    message(STATUS "oemu: lcov not found; use the `coverage-summary` target instead")
    return()
  endif()

  set(info "${CMAKE_BINARY_DIR}/coverage.info")
  set(html "${CMAKE_BINARY_DIR}/coverage-html")

  # --ignore-errors keeps lcov tolerant across gcc/lcov version mismatches.
  set(lcov_common
    --directory "${CMAKE_BINARY_DIR}"
    --rc branch_coverage=1
    --ignore-errors mismatch,unused,empty,gcov,source,negative
  )

  add_custom_target(coverage
    # Reset counters so repeated runs do not accumulate stale data.
    COMMAND "${OEMU_LCOV}" ${lcov_common} --zerocounters
    COMMAND "${CMAKE_CTEST_COMMAND}" --output-on-failure
    COMMAND "${OEMU_LCOV}" ${lcov_common} --capture --output-file "${info}.raw"
    # Keep only our own sources: drop system headers, gtest and the tests.
    COMMAND "${OEMU_LCOV}" --rc branch_coverage=1
            --ignore-errors unused,empty,negative
            --extract "${info}.raw" "${CMAKE_SOURCE_DIR}/src/*" "${CMAKE_SOURCE_DIR}/include/*"
            --output-file "${info}"
    COMMAND "${OEMU_GENHTML}" "${info}"
            --output-directory "${html}"
            --rc branch_coverage=1
            --ignore-errors source,unmapped,category
            --legend --title "oemu coverage"
    COMMAND "${CMAKE_COMMAND}" -E echo "Coverage report: ${html}/index.html"
    WORKING_DIRECTORY "${CMAKE_BINARY_DIR}"
    COMMENT "Running tests and generating lcov HTML coverage report"
    USES_TERMINAL
    VERBATIM
  )
endfunction()
