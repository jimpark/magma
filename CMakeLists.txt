cmake_minimum_required(VERSION 3.25)
project(magma VERSION 0.2.0 DESCRIPTION "Windows, input and rendering for Basalt applications"
  LANGUAGES CXX)

option(MAGMA_BUILD_TESTS "Build the Catch2 test suite" ${PROJECT_IS_TOP_LEVEL})
option(MAGMA_BUILD_BENCHMARKS "Build the Catch2 benchmarks" OFF)
option(MAGMA_INSTALL "Add install and package export rules" ${PROJECT_IS_TOP_LEVEL})
option(MAGMA_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" ${PROJECT_IS_TOP_LEVEL})
option(MAGMA_SANITIZE "Enable address and undefined-behavior sanitizers" OFF)
set(MAGMA_SDL "bundled" CACHE STRING
  "Where SDL 3 comes from: bundled (built from the pinned release) or system (find_package)")
set_property(CACHE MAGMA_SDL PROPERTY STRINGS bundled system)
if(NOT MAGMA_SDL MATCHES "^(bundled|system)$")
  message(FATAL_ERROR "MAGMA_SDL must be 'bundled' or 'system', not '${MAGMA_SDL}'.")
endif()

if(PROJECT_IS_TOP_LEVEL)
  set(CMAKE_CXX_EXTENSIONS OFF)
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
  set(CMAKE_DEBUG_POSTFIX d)
endif()

include(GNUInstallDirs)
include("${PROJECT_SOURCE_DIR}/cmake/compiler_options.cmake")
include("${PROJECT_SOURCE_DIR}/cmake/dependencies.cmake")

add_subdirectory(src)

if(MAGMA_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(MAGMA_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

if(MAGMA_INSTALL)
  include("${PROJECT_SOURCE_DIR}/cmake/install.cmake")
endif()
