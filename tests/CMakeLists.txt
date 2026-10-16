add_executable(magma_tests
  magma/frame_scheduler_test.cpp
  magma/golden.cpp
  magma/golden.h
  magma/gpu_scene_test.cpp
  magma/platform_test.cpp
  magma/render_golden_test.cpp
  magma/renderer_test.cpp)
# The platform tests push SDL events directly, and the golden-image tests read and write PNG
# files with Blend2D, so they link both as Magma does.
target_link_libraries(magma_tests PRIVATE magma::magma SDL3::SDL3 blend2d::blend2d
  Catch2::Catch2WithMain magma_compiler_options)
target_compile_definitions(magma_tests PRIVATE
  MAGMA_TEST_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data/fonts"
  MAGMA_GOLDEN_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data/golden"
  MAGMA_GOLDEN_OUTPUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/golden")
# The GPU tests use private headers, which they include as magma/render/<name>.h.
target_include_directories(magma_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)
if(MAGMA_METAL)
  target_compile_definitions(magma_tests PRIVATE MAGMA_HAS_METAL)
endif()
catch_discover_tests(magma_tests)

add_test(NAME include_boundaries
  COMMAND ${CMAKE_COMMAND} -DMAGMA_SOURCE_DIR=${PROJECT_SOURCE_DIR}
    -P ${PROJECT_SOURCE_DIR}/cmake/check_include_boundaries.cmake)
set_tests_properties(include_boundaries PROPERTIES LABELS boundaries)
