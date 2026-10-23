#pragma once

#include "basalt/color.h"
#include "basalt/image.h"

#include "magma/platform.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

#include "backend.h"

namespace magma::render {

[[nodiscard]] inline auto intersect(pixel_rect a, pixel_rect b) -> pixel_rect {
  auto const left = std::max(a.x, b.x);
  auto const top = std::max(a.y, b.y);
  auto const right = std::min(a.x + a.width, b.x + b.width);
  auto const bottom = std::min(a.y + a.height, b.y + b.height);
  return {left, top, std::max(0, right - left), std::max(0, bottom - top)};
}

[[nodiscard]] inline auto has_area(pixel_rect area) -> bool {
  return area.width > 0 && area.height > 0;
}

// The smallest rect that holds both. An empty rect adds nothing.
[[nodiscard]] inline auto union_of(pixel_rect a, pixel_rect b) -> pixel_rect {
  if (!has_area(a)) {
    return b;
  }
  if (!has_area(b)) {
    return a;
  }
  auto const left = std::min(a.x, b.x);
  auto const top = std::min(a.y, b.y);
  auto const right = std::max(a.x + a.width, b.x + b.width);
  auto const bottom = std::max(a.y + a.height, b.y + b.height);
  return {left, top, right - left, bottom - top};
}

// The color as the shaders and render targets store it: 0 to 1, with the color channels
// multiplied by alpha.
[[nodiscard]] inline auto premultiplied(basalt::rgba color) -> std::array<float, 4> {
  auto const alpha = static_cast<float>(color.a) / 255;
  return {static_cast<float>(color.r) / 255 * alpha, static_cast<float>(color.g) / 255 * alpha,
          static_cast<float>(color.b) / 255 * alpha, alpha};
}

// True when every pixel of the straight-alpha RGBA8 `pixels` has alpha 255. The loop has no early
// exit so that the compiler can vectorize it; it costs much less than premultiplying.
[[nodiscard]] inline auto is_opaque_rgba(std::span<std::uint8_t const> pixels) -> bool {
  auto alpha = std::uint8_t{255};
  for (auto offset = std::size_t{3}; offset < pixels.size(); offset += 4) {
    alpha &= pixels[offset];
  }
  return alpha == 255;
}

// Writes straight-alpha RGBA8 `pixels` to `out` with each color channel multiplied by alpha. `out`
// holds at least `pixels.size()` bytes, for example mapped staging memory. Opaque pixels do not
// change, so an opaque image is copied as it is.
inline auto write_premultiplied_rgba(std::span<std::uint8_t const> pixels, std::byte *out) -> void {
  if (is_opaque_rgba(pixels)) {
    std::memcpy(out, pixels.data(), pixels.size());
    return;
  }
  auto *const bytes = reinterpret_cast<std::uint8_t *>(out);
  for (auto offset = std::size_t{0}; offset + 3 < pixels.size(); offset += 4) {
    auto const alpha = std::uint32_t{pixels[offset + 3]};
    for (auto channel = std::size_t{0}; channel < 3; ++channel) {
      bytes[offset + channel] =
          static_cast<std::uint8_t>((std::uint32_t{pixels[offset + channel]} * alpha + 127) / 255);
    }
    bytes[offset + 3] = pixels[offset + 3];
  }
}

// A copy of straight-alpha RGBA8 pixels with each color channel multiplied by alpha.
[[nodiscard]] inline auto premultiplied_rgba(std::span<std::uint8_t const> pixels)
    -> std::vector<std::uint8_t> {
  auto result = std::vector<std::uint8_t>(pixels.size());
  write_premultiplied_rgba(pixels, reinterpret_cast<std::byte *>(result.data()));
  return result;
}

// Converts rows of premultiplied BGRA pixels to a straight-alpha image. `row_at(y)` returns the
// pointer to the `width` pixels of row `y`.
template <class row_source>
[[nodiscard]] auto to_raster_image(int width, int height, row_source &&row_at)
    -> basalt::raster_image_ptr {
  auto pixels = std::vector<std::uint8_t>(static_cast<std::size_t>(width) *
                                          static_cast<std::size_t>(height) * 4);
  auto *out = pixels.data();
  for (auto y = 0; y < height; ++y) {
    std::uint32_t const *const row = row_at(y);
    for (auto x = 0; x < width; ++x) {
      auto const color = unpremultiply(row[x]);
      *out++ = color.r;
      *out++ = color.g;
      *out++ = color.b;
      *out++ = color.a;
    }
  }
  return basalt::raster_image::create(static_cast<std::uint32_t>(width),
                                      static_cast<std::uint32_t>(height), std::move(pixels));
}

// The same, for tightly packed rows.
[[nodiscard]] inline auto to_raster_image(std::span<std::uint32_t const> pixels, int width,
                                          int height) -> basalt::raster_image_ptr {
  return to_raster_image(width, height, [&](int y) {
    return pixels.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(width);
  });
}

} // namespace magma::render
