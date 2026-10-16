#pragma once

#include "basalt/image.h"

#include <string>
#include <string_view>

namespace magma::test {

struct golden_result {
  bool matched = false;
  std::string message;
};

// How far a frame may be from its golden image. Each color channel may differ by `channel`, and
// `outlier_fraction` of the pixels may differ by more, but by no more than `outlier_channel`.
struct golden_tolerance {
  int channel = 3;
  double outlier_fraction = 0.002;
  int outlier_channel = 255;
};

// Compares `frame` with the golden image `tests/data/golden/<name>.png`. The default tolerance
// lets small differences in rounding between CPUs pass. When they do not match, the frame and an
// image of the differences are written to the build tree, and the message names them.
//
// The golden images come from the CPU renderer, which is the reference. For a GPU frame, give
// the backend's name as `backend`: the files in the build tree then have it in their names, and
// the frame never replaces the golden image.
//
// Set the environment variable MAGMA_UPDATE_GOLDEN to 1 to write the reference frame as the new
// golden image instead. Look at each new image before you commit it.
[[nodiscard]] auto compare_with_golden(std::string_view name, basalt::raster_image const &frame,
                                       golden_tolerance tolerance = {},
                                       std::string_view backend = {}) -> golden_result;

} // namespace magma::test
