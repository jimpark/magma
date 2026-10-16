# A minimal project that uses Magma the way another project would. MAGMA_CONSUME picks how:
#   package       find_package(magma) from an install prefix (set CMAKE_PREFIX_PATH)
#   subdirectory  add_subdirectory(MAGMA_SOURCE_DIR)
#   fetch         FetchContent from MAGMA_GIT_REPOSITORY at MAGMA_GIT_TAG
cmake_minimum_required(VERSION 3.25)
project(magma_consumer LANGUAGES CXX)

set(MAGMA_CONSUME "package" CACHE STRING "How to consume Magma: package, subdirectory or fetch")
set(MAGMA_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../.." CACHE PATH "Magma checkout")
set(MAGMA_GIT_REPOSITORY "https://github.com/nadiasvertex/magma.git" CACHE STRING "Magma repository")
set(MAGMA_GIT_TAG "main" CACHE STRING "Magma tag or commit")

if(MAGMA_CONSUME STREQUAL "package")
  file(STRINGS "${MAGMA_SOURCE_DIR}/CMakeLists.txt" magma_project_line
       REGEX "^project\\(magma VERSION [0-9]+\\.[0-9]+\\.[0-9]+" LIMIT_COUNT 1)
  if(NOT magma_project_line MATCHES "VERSION ([0-9]+\\.[0-9]+)\\.[0-9]+")
    message(FATAL_ERROR "No project(magma VERSION X.Y.Z) line in ${MAGMA_SOURCE_DIR}/CMakeLists.txt")
  endif()
  find_package(magma ${CMAKE_MATCH_1} CONFIG REQUIRED)
elseif(MAGMA_CONSUME STREQUAL "subdirectory")
  add_subdirectory("${MAGMA_SOURCE_DIR}" magma)
elseif(MAGMA_CONSUME STREQUAL "fetch")
  include(FetchContent)
  FetchContent_Declare(magma GIT_REPOSITORY "${MAGMA_GIT_REPOSITORY}" GIT_TAG "${MAGMA_GIT_TAG}")
  FetchContent_MakeAvailable(magma)
else()
  message(FATAL_ERROR "Unknown MAGMA_CONSUME '${MAGMA_CONSUME}'.")
endif()

add_executable(consumer main.cpp)
target_link_libraries(consumer PRIVATE magma::magma)

enable_testing()
add_test(NAME consumer COMMAND consumer)
# SDL's dummy video driver needs no display.
set_tests_properties(consumer PROPERTIES ENVIRONMENT SDL_VIDEO_DRIVER=dummy)
