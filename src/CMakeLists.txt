# The windowing and rendering library. SDL and Blend2D are private: no public header includes them.
add_library(magma STATIC
  magma/platform/display_link.h
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
target_include_directories(magma PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(magma
  PUBLIC basalt::basalt
  PRIVATE SDL3::SDL3 blend2d::blend2d $<BUILD_INTERFACE:magma_compiler_options>)
set_target_properties(magma PROPERTIES EXPORT_NAME magma)

# The display link follows the display's refresh on macOS. Other platforms pace frames with a
# timer at the display's refresh rate.
if(MAGMA_METAL)
  target_sources(magma PRIVATE magma/platform/display_link_macos.mm magma/platform/media_clock.h)
  set_source_files_properties(magma/platform/display_link_macos.mm PROPERTIES
    COMPILE_OPTIONS -fobjc-arc)
  target_link_libraries(magma PRIVATE "-framework AppKit")
else()
  target_sources(magma PRIVATE magma/platform/display_link_stub.cpp)
endif()

# Metal is the GPU backend on macOS. Its shaders are compiled when the renderer starts, so the
# build needs no Metal toolchain.
if(MAGMA_METAL)
  target_sources(magma PRIVATE magma/render/metal_backend.mm)
  set_source_files_properties(magma/render/metal_backend.mm PROPERTIES COMPILE_OPTIONS -fobjc-arc)
  target_compile_definitions(magma PRIVATE MAGMA_HAS_METAL)
  target_link_libraries(magma PRIVATE "-framework Foundation" "-framework Metal"
    "-framework QuartzCore")
endif()

# The Vulkan backend loads the Vulkan library when it starts, through volk, which is compiled into
# Magma. The shaders are compiled to SPIR-V at build time and included as arrays.
if(MAGMA_VULKAN)
  set(_shader_dir "${CMAKE_CURRENT_SOURCE_DIR}/magma/render/shaders")
  set(_shader_output_dir "${CMAKE_CURRENT_BINARY_DIR}/shaders")
  file(MAKE_DIRECTORY "${_shader_output_dir}")
  set(_spirv_headers "")
  foreach(_stage IN ITEMS vert frag)
    if(_stage STREQUAL "vert")
      set(_variable vertex_spirv)
    else()
      set(_variable fragment_spirv)
    endif()
    set(_header "${_shader_output_dir}/gpu_${_stage}_spirv.h")
    add_custom_command(
      OUTPUT "${_header}"
      COMMAND glslang-standalone -V --target-env vulkan1.0 --vn ${_variable}
              -o "${_header}" "${_shader_dir}/gpu.${_stage}"
      DEPENDS glslang-standalone "${_shader_dir}/gpu.${_stage}"
      COMMENT "Compiling gpu.${_stage} to SPIR-V"
      VERBATIM)
    list(APPEND _spirv_headers "${_header}")
  endforeach()
  add_custom_target(magma_shaders DEPENDS ${_spirv_headers})
  add_dependencies(magma magma_shaders)

  target_sources(magma PRIVATE magma/render/vulkan_backend.cpp "${volk_SOURCE_DIR}/volk.c")
  # volk is third-party C code, so Magma's warnings do not apply to it.
  set_source_files_properties("${volk_SOURCE_DIR}/volk.c" PROPERTIES COMPILE_OPTIONS
    "$<IF:$<BOOL:${MSVC}>,/w,-w>")
  target_include_directories(magma SYSTEM PRIVATE "${vulkan_headers_SOURCE_DIR}/include"
    "${volk_SOURCE_DIR}")
  target_include_directories(magma PRIVATE "${_shader_output_dir}")
  target_compile_definitions(magma PRIVATE MAGMA_HAS_VULKAN)
  if(NOT WIN32)
    target_link_libraries(magma PRIVATE ${CMAKE_DL_LIBS})
  endif()
endif()

# The Direct3D 12 backend compiles its HLSL shader when it starts, like the Metal backend, so the
# build needs no shader compiler. The source is included as a string.
if(MAGMA_D3D12)
  set(_hlsl_file "${CMAKE_CURRENT_SOURCE_DIR}/magma/render/shaders/gpu.hlsl")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_hlsl_file}")
  file(READ "${_hlsl_file}" _hlsl_source)
  file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/shaders/gpu_hlsl.h"
    "#pragma once\n\nconstexpr char const hlsl_source[] = R\"hlsl(${_hlsl_source})hlsl\";\n")
  target_sources(magma PRIVATE magma/render/d3d12_backend.cpp)
  target_include_directories(magma PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/shaders")
  target_compile_definitions(magma PRIVATE MAGMA_HAS_D3D12)
  target_link_libraries(magma PRIVATE d3d12 dxgi d3dcompiler)
endif()
