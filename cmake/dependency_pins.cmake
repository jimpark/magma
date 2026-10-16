# Every fetched dependency, pinned to a release archive and its SHA-256. Updating a
# dependency is a two-line review here. Set FETCHCONTENT_SOURCE_DIR_<NAME> to use a
# local checkout instead, for example in offline builds.
include_guard(GLOBAL)

set(MAGMA_BASALT_VERSION "0.6.0")
set(MAGMA_BASALT_URL
  "https://github.com/nadiasvertex/basalt/archive/refs/tags/v${MAGMA_BASALT_VERSION}.tar.gz")
set(MAGMA_BASALT_SHA256 "daf99cdf3f25867a42737661090f3b8ae5be08f8471ce7ae1e4dced37e394c36")

set(MAGMA_SDL3_VERSION "3.4.18")
set(MAGMA_SDL3_URL
  "https://github.com/libsdl-org/SDL/releases/download/release-${MAGMA_SDL3_VERSION}/SDL3-${MAGMA_SDL3_VERSION}.tar.gz")
set(MAGMA_SDL3_SHA256 "9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3")

# The source release from blend2d.com includes asmjit, which Blend2D's JIT compiler needs.
set(MAGMA_BLEND2D_VERSION "0.21.2")
set(MAGMA_BLEND2D_URL "https://blend2d.com/download/blend2d-${MAGMA_BLEND2D_VERSION}.tar.gz")
set(MAGMA_BLEND2D_SHA256 "234ba57a64153c96885a6271f15fdcf6c594ac8cb03adaad19abb8d3b2b533fa")

set(MAGMA_CATCH2_VERSION "3.16.0")
set(MAGMA_CATCH2_URL
  "https://github.com/catchorg/Catch2/archive/refs/tags/v${MAGMA_CATCH2_VERSION}.tar.gz")
set(MAGMA_CATCH2_SHA256 "0957cae5821b17ce07f0833aaa52b5137643a8382203221f363a8303c109af34")
