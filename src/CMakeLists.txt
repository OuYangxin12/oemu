# The library itself. Split into modules so each one can own its
# <module>_internal.h without leaking it into the public include path.

add_library(oemu
  core/allocator.c
  core/check.c
  core/status.c
  core/version.c
  buffer/buffer.c
)

# Internal headers, listed so IDEs pick them up.
target_sources(oemu PRIVATE
  buffer/buffer_internal.h
)

target_include_directories(oemu
  PUBLIC
    # Public headers: visible to consumers and to the tests.
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    # Lets buffer.c say #include "buffer_internal.h".
    ${CMAKE_CURRENT_SOURCE_DIR}/buffer
)

target_link_libraries(oemu PUBLIC oemu_compiler_flags)

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
