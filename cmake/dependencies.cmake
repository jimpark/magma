# Third-party dependencies: Basalt, SDL 3 for windows and input, Blend2D for raster rendering, and
# Catch2 for the tests and benchmarks.
include(FetchContent)
include("${CMAKE_CURRENT_LIST_DIR}/dependency_pins.cmake")

# An installed Basalt is used when one is found; otherwise the pinned release is built. A parent
# project that declares basalt first decides which Basalt is used.
set(BASALT_BUILD_TESTS OFF)
set(BASALT_BUILD_TOOLS OFF)
set(BASALT_INSTALL ${MAGMA_INSTALL})
FetchContent_Declare(basalt
  URL "${MAGMA_BASALT_URL}"
  URL_HASH "SHA256=${MAGMA_BASALT_SHA256}"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  FIND_PACKAGE_ARGS ${MAGMA_BASALT_VERSION} CONFIG)
FetchContent_MakeAvailable(basalt)

if(MAGMA_SDL STREQUAL "bundled")
  # A static SDL is linked into every program that uses Magma. When Magma is installed, SDL is
  # installed beside it so that the CMake package can find it.
  set(SDL_SHARED OFF)
  set(SDL_STATIC ON)
  set(SDL_TEST_LIBRARY OFF)
  set(SDL_TESTS OFF)
  set(SDL_EXAMPLES OFF)
  set(SDL_INSTALL ${MAGMA_INSTALL})
  FetchContent_Declare(SDL3
    URL "${MAGMA_SDL3_URL}"
    URL_HASH "SHA256=${MAGMA_SDL3_SHA256}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_MakeAvailable(SDL3)
else()
  find_package(SDL3 3.2 CONFIG REQUIRED)
endif()

# A static Blend2D is linked into every program that uses Magma, and installed beside it like SDL.
# Its headers are system headers, so its code does not trigger Magma's warnings.
set(BLEND2D_STATIC ON)
if(MAGMA_INSTALL)
  set(BLEND2D_NO_INSTALL OFF)
else()
  set(BLEND2D_NO_INSTALL ON)
endif()
FetchContent_Declare(blend2d
  URL "${MAGMA_BLEND2D_URL}"
  URL_HASH "SHA256=${MAGMA_BLEND2D_SHA256}"
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
  SYSTEM)
FetchContent_MakeAvailable(blend2d)

if(MAGMA_BUILD_TESTS OR MAGMA_BUILD_BENCHMARKS)
  # An installed Catch2 3 is used when one is found; otherwise the pinned release is fetched.
  FetchContent_Declare(Catch2
    URL "${MAGMA_CATCH2_URL}"
    URL_HASH "SHA256=${MAGMA_CATCH2_SHA256}"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    FIND_PACKAGE_ARGS 3)
  # Catch2 enables its std::string_view support from the language level it is compiled with,
  # which must match the tests.
  block(SCOPE_FOR VARIABLES PROPAGATE catch2_SOURCE_DIR Catch2_DIR)
    set(CMAKE_CXX_STANDARD 23)
    FetchContent_MakeAvailable(Catch2)
  endblock()
  # Catch.cmake provides catch_discover_tests.
  if(catch2_SOURCE_DIR)
    list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
  elseif(Catch2_DIR)
    list(APPEND CMAKE_MODULE_PATH "${Catch2_DIR}")
  endif()
  include(Catch)
endif()
