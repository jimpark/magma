add_executable(magma_tests
  magma/frame_scheduler_test.cpp
  magma/platform_test.cpp)
# The platform tests push SDL events directly, so they link SDL as Magma does.
target_link_libraries(magma_tests PRIVATE magma::magma SDL3::SDL3 Catch2::Catch2WithMain
  magma_compiler_options)
catch_discover_tests(magma_tests)

add_test(NAME include_boundaries
  COMMAND ${CMAKE_COMMAND} -DMAGMA_SOURCE_DIR=${PROJECT_SOURCE_DIR}
    -P ${PROJECT_SOURCE_DIR}/cmake/check_include_boundaries.cmake)
set_tests_properties(include_boundaries PROPERTIES LABELS boundaries)
