# Installs the library, its headers and a CMake package, so that consumers can use
# find_package(magma) against an install prefix or an unpacked release archive. A bundled SDL and
# a fetched Basalt and Blend2D install their own packages into the same prefix.
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(MAGMA_CMAKE_DIR "${CMAKE_INSTALL_LIBDIR}/cmake/magma")

install(TARGETS magma EXPORT magmaTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(EXPORT magmaTargets NAMESPACE magma:: DESTINATION ${MAGMA_CMAKE_DIR})

configure_package_config_file(cmake/magmaConfig.cmake.in
  "${PROJECT_BINARY_DIR}/magmaConfig.cmake"
  INSTALL_DESTINATION ${MAGMA_CMAKE_DIR})
# Before 1.0, a minor version may break the API.
write_basic_package_version_file("${PROJECT_BINARY_DIR}/magmaConfigVersion.cmake"
  COMPATIBILITY SameMinorVersion)
install(FILES
  "${PROJECT_BINARY_DIR}/magmaConfig.cmake"
  "${PROJECT_BINARY_DIR}/magmaConfigVersion.cmake"
  DESTINATION ${MAGMA_CMAKE_DIR})
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/magma)
install(FILES "${blend2d_SOURCE_DIR}/LICENSE.md" DESTINATION ${CMAKE_INSTALL_DATADIR}/doc/magma
  RENAME blend2d-LICENSE.md)
