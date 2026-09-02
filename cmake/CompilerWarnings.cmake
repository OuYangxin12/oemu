# Centralised warning configuration.
#
# Applied to an INTERFACE target that every other target links, so production
# code and tests share one definition of "clean build".

function(oemu_set_project_warnings target)
  set(gcc_clang_common
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wcast-qual
    -Wcast-align
    -Wconversion
    -Wsign-conversion
    -Wdouble-promotion
    -Wformat=2
    -Wundef
    -Wwrite-strings
    -Wpointer-arith
    -Wredundant-decls
    -Wswitch-enum
    -Wunreachable-code
  )

  # C-only warnings: these have no meaning for the C++ test translation units.
  set(c_only
    -Wstrict-prototypes
    -Wold-style-definition
    -Wmissing-prototypes
    -Wmissing-declarations
    -Wbad-function-cast
    -Wnested-externs
  )

  if(CMAKE_C_COMPILER_ID STREQUAL "GNU")
    list(APPEND gcc_clang_common
      -Wlogical-op
      -Wduplicated-cond
      -Wduplicated-branches
    )
    # C-only in GCC: emits a "valid for C but not for C++" note otherwise.
    list(APPEND c_only -Wjump-misses-init)
  endif()

  set(msvc_warnings /W4 /permissive-)

  if(MSVC)
    set(warnings ${msvc_warnings})
    if(OEMU_WARNINGS_AS_ERRORS)
      list(APPEND warnings /WX)
    endif()
    target_compile_options(${target} INTERFACE ${warnings})
  else()
    if(OEMU_WARNINGS_AS_ERRORS)
      list(APPEND gcc_clang_common -Werror)
    endif()
    # $<COMPILE_LANGUAGE:C> keeps the C-only flags away from the C++ tests.
    target_compile_options(${target} INTERFACE
      ${gcc_clang_common}
      "$<$<COMPILE_LANGUAGE:C>:${c_only}>"
    )
  endif()
endfunction()
