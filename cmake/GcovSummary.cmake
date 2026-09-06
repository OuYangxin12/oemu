# Script-mode helper: prints a per-file line-coverage summary using plain gcov.
#
# Invoked by the `coverage-summary` target. Exists so coverage numbers are
# available on machines without lcov/genhtml.
#
# Expects: GCOV_EXECUTABLE, BINARY_DIR, SOURCE_DIR.

if(NOT GCOV_EXECUTABLE OR NOT BINARY_DIR OR NOT SOURCE_DIR)
  message(FATAL_ERROR "GcovSummary.cmake: GCOV_EXECUTABLE, BINARY_DIR and SOURCE_DIR are required")
endif()

# .gcda files only exist for translation units that actually ran.
file(GLOB_RECURSE gcda_files "${BINARY_DIR}/*.gcda")

if(NOT gcda_files)
  message(FATAL_ERROR
    "No .gcda files found under ${BINARY_DIR}.\n"
    "Configure with -DOEMU_ENABLE_COVERAGE=ON and run the tests first.")
endif()

set(report_dir "${BINARY_DIR}/gcov-report")
file(REMOVE_RECURSE "${report_dir}")
file(MAKE_DIRECTORY "${report_dir}")

# Run gcov once per .gcda; -b adds branch data, -p keeps path-mangled names so
# same-named files in different directories do not collide.
#
# The .gcda set includes every test TU, so the sweep is fanned out through
# xargs instead of running one file at a time. Paths are newline-joined and
# split with -d, so build directories containing spaces stay one argument.
# Falls back to a plain loop where xargs is unavailable.
find_program(OEMU_XARGS xargs)

if(OEMU_XARGS)
  cmake_host_system_information(RESULT oemu_jobs QUERY NUMBER_OF_LOGICAL_CORES)
  string(JOIN "\n" oemu_gcda_lines ${gcda_files})
  file(WRITE "${report_dir}/gcda.list" "${oemu_gcda_lines}\n")
  execute_process(
    COMMAND "${OEMU_XARGS}" -a "${report_dir}/gcda.list" -d "\n"
            -P "${oemu_jobs}" -n 1 "${GCOV_EXECUTABLE}" -b -c -p
    WORKING_DIRECTORY "${report_dir}"
    OUTPUT_VARIABLE _out
    ERROR_VARIABLE _err
    RESULT_VARIABLE _rc
  )
  if(NOT _rc EQUAL 0)
    # xargs exits 123 when any invocation failed; the count below reports how
    # bad it was, so the message here only needs to point at the cause.
    message(WARNING "gcov sweep had failures (rc=${_rc}): ${_err}")
  endif()
else()
  foreach(gcda IN LISTS gcda_files)
    execute_process(
      COMMAND "${GCOV_EXECUTABLE}" -b -c -p "${gcda}"
      WORKING_DIRECTORY "${report_dir}"
      OUTPUT_VARIABLE _out
      ERROR_VARIABLE _err
      RESULT_VARIABLE _rc
    )
    if(NOT _rc EQUAL 0)
      message(WARNING "gcov failed for ${gcda}: ${_err}")
    endif()
  endforeach()
endif()

file(GLOB gcov_files "${report_dir}/*.gcov")

# A silent empty run is worse than a failure: the summary would print "no
# project lines instrumented" and exit 0 while CI and humans read it as real.
# By far the most common cause is a gcov that cannot parse the profile format
# of the compiler which wrote the .gcda files.
if(NOT gcov_files)
  message(FATAL_ERROR
    "GcovSummary.cmake: gcov produced no .gcov files, so there is nothing to "
    "report. The usual cause is a version mismatch: ${GCOV_EXECUTABLE} cannot "
    "parse the profiles written by the configured compiler. Compare "
    "\"${GCOV_EXECUTABLE} --version\" with the compiler's version.")
endif()

set(total_lines 0)
set(total_covered 0)
set(rows "")

foreach(gcov_file IN LISTS gcov_files)
  file(STRINGS "${gcov_file}" lines)

  # The first line records the source path as "        -:    0:Source:<path>".
  set(source_path "")
  foreach(line IN LISTS lines)
    if(line MATCHES "0:Source:(.+)$")
      set(source_path "${CMAKE_MATCH_1}")
      break()
    endif()
  endforeach()

  if(NOT source_path)
    continue()
  endif()

  get_filename_component(source_abs "${source_path}" ABSOLUTE)

  # Report on project sources only; skip system headers, gtest and the tests.
  string(FIND "${source_abs}" "${SOURCE_DIR}/src" in_src)
  string(FIND "${source_abs}" "${SOURCE_DIR}/include" in_include)
  if(in_src EQUAL -1 AND in_include EQUAL -1)
    continue()
  endif()

  set(file_lines 0)
  set(file_covered 0)
  foreach(line IN LISTS lines)
    # Executable lines start with a hit count or "#####" for never-executed.
    if(line MATCHES "^ *([0-9]+)\\*?: *[0-9]+:")
      math(EXPR file_lines "${file_lines} + 1")
      math(EXPR file_covered "${file_covered} + 1")
    elseif(line MATCHES "^ *[#=]+: *[0-9]+:")
      math(EXPR file_lines "${file_lines} + 1")
    endif()
  endforeach()

  if(file_lines EQUAL 0)
    continue()
  endif()

  math(EXPR pct "(${file_covered} * 100) / ${file_lines}")
  file(RELATIVE_PATH rel "${SOURCE_DIR}" "${source_abs}")

  string(LENGTH "${rel}" rel_len)
  if(rel_len LESS 44)
    string(REPEAT " " 44 pad)
    string(SUBSTRING "${rel}${pad}" 0 44 rel_padded)
  else()
    set(rel_padded "${rel}")
  endif()

  list(APPEND rows "  ${rel_padded} ${pct}%  (${file_covered}/${file_lines} lines)")

  math(EXPR total_lines "${total_lines} + ${file_lines}")
  math(EXPR total_covered "${total_covered} + ${file_covered}")
endforeach()

list(SORT rows)

message("")
message("Line coverage by file")
message("---------------------")
foreach(row IN LISTS rows)
  message("${row}")
endforeach()

if(total_lines GREATER 0)
  math(EXPR total_pct "(${total_covered} * 100) / ${total_lines}")
  message("---------------------")
  message("  TOTAL: ${total_pct}%  (${total_covered}/${total_lines} lines)")
else()
  message("  no project lines instrumented")
endif()
message("")
message("Raw .gcov files: ${report_dir}")
message("For an HTML report install lcov and build the `coverage` target.")
message("")
