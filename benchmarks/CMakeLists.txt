if(MAGMA_SANITIZE)
  message(FATAL_ERROR "Benchmarks measure the cost of instrumentation under MAGMA_SANITIZE. "
    "Configure without it.")
endif()
if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo)$")
  message(WARNING "Benchmarks need an optimized build. CMAKE_BUILD_TYPE is "
    "'${CMAKE_BUILD_TYPE}'; use the benchmark preset.")
endif()

add_executable(magma_benchmarks render_benchmark.cpp)
target_link_libraries(magma_benchmarks PRIVATE magma::magma Catch2::Catch2WithMain
  magma_compiler_options)
target_compile_definitions(magma_benchmarks PRIVATE
  MAGMA_TEST_FONT_DIR="${PROJECT_SOURCE_DIR}/tests/data/fonts")
