# The windowing and rendering library. SDL and Blend2D are private: no public header includes them.
add_library(magma STATIC
  magma/platform/sdl_platform.cpp
  magma/platform/sdl_platform_internal.h
  magma/platform/sdl_window.cpp
  magma/render/backend.h
  magma/render/blend2d_painter.cpp
  magma/render/blend2d_painter.h
  magma/render/glyph_atlas.cpp
  magma/render/glyph_atlas.h
  magma/render/gpu_scene.cpp
  magma/render/gpu_scene.h
  magma/render/raster_backend.cpp
  magma/render/renderer.cpp)
add_library(magma::magma ALIAS magma)

target_compile_features(magma PUBLIC cxx_std_23)
target_include_directories(magma PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(magma
  PUBLIC basalt::basalt
  PRIVATE SDL3::SDL3 blend2d::blend2d $<BUILD_INTERFACE:magma_compiler_options>)
set_target_properties(magma PROPERTIES EXPORT_NAME magma)

# Metal is the GPU backend on macOS. Its shaders are compiled when the renderer starts, so the
# build needs no Metal toolchain.
if(MAGMA_METAL)
  target_sources(magma PRIVATE magma/render/metal_backend.mm)
  set_source_files_properties(magma/render/metal_backend.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
  target_compile_definitions(magma PRIVATE MAGMA_HAS_METAL)
  target_link_libraries(magma PRIVATE "-framework Foundation" "-framework Metal"
    "-framework QuartzCore")
endif()
