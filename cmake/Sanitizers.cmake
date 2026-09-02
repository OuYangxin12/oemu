# Sanitizer wiring driven by the OEMU_SANITIZERS cache variable.
#
#   cmake -DOEMU_SANITIZERS="address;undefined" ...
#
# Flags are attached to an INTERFACE target for both compiling and linking,
# which is required: sanitizers need the runtime linked in as well.

function(oemu_enable_sanitizers target)
  if(NOT OEMU_SANITIZERS)
    return()
  endif()

  if(MSVC)
    if("address" IN_LIST OEMU_SANITIZERS)
      target_compile_options(${target} INTERFACE /fsanitize=address)
    endif()
    return()
  endif()

  set(requested "")
  foreach(san IN LISTS OEMU_SANITIZERS)
    string(TOLOWER "${san}" san)
    if(san STREQUAL "address" OR san STREQUAL "asan")
      list(APPEND requested address)
    elseif(san STREQUAL "undefined" OR san STREQUAL "ubsan")
      list(APPEND requested undefined)
    elseif(san STREQUAL "thread" OR san STREQUAL "tsan")
      list(APPEND requested thread)
    elseif(san STREQUAL "leak" OR san STREQUAL "lsan")
      list(APPEND requested leak)
    elseif(san STREQUAL "memory" OR san STREQUAL "msan")
      list(APPEND requested memory)
    else()
      message(FATAL_ERROR "oemu: unknown sanitizer '${san}'")
    endif()
  endforeach()

  list(REMOVE_DUPLICATES requested)

  # ThreadSanitizer is incompatible with ASan/LSan; catch the mistake early.
  if("thread" IN_LIST requested AND ("address" IN_LIST requested OR "leak" IN_LIST requested))
    message(FATAL_ERROR "oemu: thread sanitizer cannot be combined with address/leak")
  endif()

  string(REPLACE ";" "," san_arg "${requested}")

  set(flags
    -fsanitize=${san_arg}
    -fno-omit-frame-pointer
    -fno-optimize-sibling-calls
    -g
  )
  if("undefined" IN_LIST requested)
    # Turn UB reports into hard failures so ctest actually fails the run.
    list(APPEND flags -fno-sanitize-recover=all)
  endif()

  target_compile_options(${target} INTERFACE ${flags})
  target_link_options(${target} INTERFACE -fsanitize=${san_arg})

  # ThreadSanitizer aborts with "unexpected memory mapping" on kernels that use
  # more ASLR entropy than its shadow-memory layout expects (Linux 6.x, WSL2).
  # Running the test binaries through `setarch -R` disables randomisation for
  # them, which is the standard workaround. Exported for tests/CMakeLists.txt.
  if("thread" IN_LIST requested AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_program(OEMU_SETARCH setarch)
    if(OEMU_SETARCH)
      set(OEMU_TEST_LAUNCHER "${OEMU_SETARCH}" "-R" CACHE INTERNAL
        "Launcher prefix required to run sanitized test binaries")
      message(STATUS "oemu: TSan detected; running tests via `setarch -R` to disable ASLR")
    else()
      message(WARNING
        "oemu: TSan needs `setarch` to disable ASLR on this kernel. "
        "Install util-linux or lower /proc/sys/vm/mmap_rnd_bits to 28.")
    endif()
  endif()

  message(STATUS "oemu: sanitizers enabled -> ${san_arg}")
endfunction()
