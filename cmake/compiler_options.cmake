# Warning policy and sanitizer wiring for Magma's own targets. Linked PRIVATE, so none of it
# reaches consumers.
add_library(magma_compiler_options INTERFACE)

if(MSVC)
  target_compile_options(magma_compiler_options INTERFACE /W4 /utf-8)
  if(MAGMA_WARNINGS_AS_ERRORS)
    target_compile_options(magma_compiler_options INTERFACE /WX)
  endif()
else()
  target_compile_options(magma_compiler_options INTERFACE -Wall)
  if(MAGMA_WARNINGS_AS_ERRORS)
    target_compile_options(magma_compiler_options INTERFACE -Werror)
  endif()
endif()

if(MAGMA_SANITIZE AND NOT MSVC)
  target_compile_options(magma_compiler_options INTERFACE
    -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
  target_link_options(magma_compiler_options INTERFACE -fsanitize=address,undefined)
endif()
