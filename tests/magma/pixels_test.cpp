#include <catch2/catch_test_macros.hpp>

#include "magma/render/pixels.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

using magma::render::is_opaque_rgba;
using magma::render::premultiplied_rgba;
using magma::render::write_premultiplied_rgba;

TEST_CASE("an image is opaque only when every alpha is 255", "[pixels]") {
  CHECK(is_opaque_rgba(std::array<std::uint8_t, 8>{1, 2, 3, 255, 4, 5, 6, 255}));
  CHECK_FALSE(is_opaque_rgba(std::array<std::uint8_t, 8>{1, 2, 3, 255, 4, 5, 6, 254}));
  CHECK_FALSE(is_opaque_rgba(std::array<std::uint8_t, 8>{1, 2, 3, 0, 4, 5, 6, 255}));
}

TEST_CASE("opaque pixels are written unchanged", "[pixels]") {
  auto const pixels = std::array<std::uint8_t, 8>{10, 20, 30, 255, 200, 100, 0, 255};
  auto out = std::array<std::byte, 8>{};
  write_premultiplied_rgba(pixels, out.data());
  for (auto index = std::size_t{0}; index < pixels.size(); ++index) {
    CHECK(std::to_integer<std::uint8_t>(out[index]) == pixels[index]);
  }
}

TEST_CASE("translucent pixels are written with color times alpha, rounded", "[pixels]") {
  // The opaque first pixel must not make the image count as opaque.
  auto const pixels = std::array<std::uint8_t, 12>{10, 20, 30, 255, 200, 100, 1, 128, 9, 9, 9, 0};
  auto out = std::array<std::byte, 12>{};
  write_premultiplied_rgba(pixels, out.data());
  auto const expected = std::array<std::uint8_t, 12>{10, 20, 30, 255, 100, 50, 1, 128, 0, 0, 0, 0};
  for (auto index = std::size_t{0}; index < expected.size(); ++index) {
    CHECK(std::to_integer<std::uint8_t>(out[index]) == expected[index]);
  }
  CHECK(premultiplied_rgba(pixels) == std::vector<std::uint8_t>(expected.begin(), expected.end()));
}
