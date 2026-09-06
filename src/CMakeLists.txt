# The library itself. Split into modules so each one can own its
# <module>_internal.h without leaking it into the public include path.

add_library(oemu
  core/allocator.c
  core/check.c
  core/status.c
  core/sysenv.c
  core/version.c
  buffer/buffer.c
  regs/regs.c
  decode/decode.c
  memory/memory.c
  aspace/aspace.c
  machine/machine.c
  sysreg/sysreg.c
  exec/exec.c
  elf/elf.c
)

# Internal headers, listed so IDEs pick them up.
target_sources(oemu PRIVATE
  buffer/buffer_internal.h
  regs/regs_internal.h
  decode/decode_internal.h
  memory/memory_internal.h
  aspace/aspace_internal.h
  exec/exec_internal.h
  elf/elf_internal.h
  sysreg/sysreg_internal.h
)

target_include_directories(oemu
  PUBLIC
    # Public headers: visible to consumers and to the tests.
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    # Lets buffer.c say #include "buffer_internal.h".
    ${CMAKE_CURRENT_SOURCE_DIR}/buffer
    # Likewise for regs.c and "regs_internal.h".
    ${CMAKE_CURRENT_SOURCE_DIR}/regs
    # Likewise for decode.c and "decode_internal.h".
    ${CMAKE_CURRENT_SOURCE_DIR}/decode
    # Likewise for memory.c / exec.c and their internal headers.
    ${CMAKE_CURRENT_SOURCE_DIR}/memory
    # Likewise for aspace.c and "aspace_internal.h".
    ${CMAKE_CURRENT_SOURCE_DIR}/aspace
    ${CMAKE_CURRENT_SOURCE_DIR}/exec
    # Likewise for elf.c and "elf_internal.h".
    ${CMAKE_CURRENT_SOURCE_DIR}/elf
    # Likewise for sysreg.c and "sysreg_internal.h".
    ${CMAKE_CURRENT_SOURCE_DIR}/sysreg
)

target_link_libraries(oemu PUBLIC oemu_compiler_flags)

# sysenv.c is the one file that needs a POSIX symbol (clock_gettime). The
# feature-test macro is supplied here rather than in the source so the source
# stays free of a reserved _-prefixed macro definition (cert-dcl37-c).
set_source_files_properties(core/sysenv.c PROPERTIES COMPILE_DEFINITIONS _POSIX_C_SOURCE=2008090L)

set_target_properties(oemu PROPERTIES
  VERSION ${PROJECT_VERSION}
  SOVERSION ${PROJECT_VERSION_MAJOR}
  C_VISIBILITY_PRESET hidden
)

# Exposes the module source dirs to the test targets so a white-box test can
# include "buffer/buffer_internal.h". Deliberately a plain variable promoted to
# the parent scope rather than a PUBLIC include dir: production consumers of
# the library must not see internal headers.
set(OEMU_INTERNAL_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" PARENT_SCOPE)

# --- optional demo executable -------------------------------------------------

add_executable(oemu_cli main.c)
target_link_libraries(oemu_cli PRIVATE oemu oemu_compiler_flags)
set_target_properties(oemu_cli PROPERTIES OUTPUT_NAME oemu)

# --- install rules ------------------------------------------------------------

include(GNUInstallDirs)

install(TARGETS oemu
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/include/oemu
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
