#pragma once

#include "basalt/image.h"

#include <string>
#include <string_view>

namespace magma::test {

struct golden_result {
  bool matched = false;
  std::string message;
};

// Compares `frame` with the golden image `tests/data/golden/<name>.png`. Each color channel may
// differ by a small amount, and a few pixels may differ by more, so that small differences in
// rounding between CPUs do not fail the test. When they do not match, the frame and an image of
// the differences are written to the build tree, and the message names them.
//
// Set the environment variable MAGMA_UPDATE_GOLDEN to 1 to write the frame as the new golden
// image instead. Look at each new image before you commit it.
[[nodiscard]] auto compare_with_golden(std::string_view name, basalt::raster_image const &frame)
    -> golden_result;

} // namespace magma::test
