# The windowing library. SDL is private: no public header includes it.
add_library(magma STATIC
  magma/platform/sdl_platform.cpp
  magma/platform/sdl_platform_internal.h
  magma/platform/sdl_window.cpp)
add_library(magma::magma ALIAS magma)

target_compile_features(magma PUBLIC cxx_std_23)
target_include_directories(magma PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(magma
  PUBLIC basalt::basalt
  PRIVATE SDL3::SDL3 $<BUILD_INTERFACE:magma_compiler_options>)
set_target_properties(magma PROPERTIES EXPORT_NAME magma)
