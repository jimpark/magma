#include <blend2d/blend2d.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "backend.h"
#include "blend2d_painter.h"

namespace magma::render {
namespace {

// Drawing on several threads pays off only for large areas: below about this many pixels the
// threads cost more to coordinate than they save.
constexpr auto threaded_area = std::int64_t{512} * 512;
constexpr auto most_threads = 4U;

auto thread_count(std::span<pixel_rect const> areas) -> std::uint32_t {
  auto pixels = std::int64_t{0};
  for (auto const &area : areas) {
    pixels += std::int64_t{area.width} * area.height;
  }
  if (pixels < threaded_area) {
    return 0;
  }
  return std::min(most_threads, std::thread::hardware_concurrency());
}

class raster_backend final : public render_backend {
public:
  raster_backend(platform *platform, window_id window) : platform_(platform), window_(window) {}

  [[nodiscard]] auto kind() const -> backend override { return backend::raster; }

  [[nodiscard]] auto draw(frame_request const &request)
      -> std::expected<void, render_error> override {
    if (frame_.width() != request.width || frame_.height() != request.height) {
      if (frame_.create(request.width, request.height, BL_FORMAT_PRGB32) != BL_SUCCESS) {
        frame_.reset();
        return std::unexpected(render_error{"could not create a frame of that size"});
      }
    }
    auto info = BLContextCreateInfo{};
    info.thread_count = thread_count(request.areas);
    auto context = BLContext{};
    if (context.begin(frame_, info) != BL_SUCCESS) {
      return std::unexpected(render_error{"could not start drawing the frame"});
    }
    for (auto const &area : request.areas) {
      context.save();
      context.clip_to_rect(BLRectI{area.x, area.y, area.width, area.height});
      context.set_comp_op(BL_COMP_OP_SRC_COPY);
      context.fill_all(
          BLRgba32(request.clear.r, request.clear.g, request.clear.b, request.clear.a));
      context.set_comp_op(BL_COMP_OP_SRC_OVER);
      painter_.replay(context, BLBoxI{area.x, area.y, area.x + area.width, area.y + area.height},
                      request.scale, request.commands);
      context.restore();
    }
    context.end();

    if (platform_ == nullptr) {
      return {};
    }
    auto data = BLImageData{};
    frame_.get_data(&data);
    auto const row_bytes = static_cast<std::size_t>(data.stride);
    auto const pixels = std::span{static_cast<std::byte const *>(data.pixel_data),
                                  row_bytes * static_cast<std::size_t>(request.height)};
    auto const damage = request.full_redraw ? std::span<pixel_rect const>{} : request.areas;
    if (auto presented = platform_->present_pixels(window_, pixels, request.width, request.height,
                                                   row_bytes, damage);
        !presented) {
      return std::unexpected(render_error{std::move(presented.error().message)});
    }
    return {};
  }

  [[nodiscard]] auto read_pixel(int x, int y) const -> std::optional<basalt::rgba> override {
    if (frame_.is_empty() || x < 0 || y < 0 || x >= frame_.width() || y >= frame_.height()) {
      return std::nullopt;
    }
    auto data = BLImageData{};
    frame_.get_data(&data);
    auto const *const row = static_cast<std::uint8_t const *>(data.pixel_data) + y * data.stride;
    return unpremultiply(reinterpret_cast<std::uint32_t const *>(row)[x]);
  }

  [[nodiscard]] auto read_frame() const -> basalt::raster_image_ptr override {
    if (frame_.is_empty()) {
      return nullptr;
    }
    auto data = BLImageData{};
    frame_.get_data(&data);
    auto const width = frame_.width();
    auto const height = frame_.height();
    auto pixels = std::vector<std::uint8_t>{};
    pixels.reserve(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4);
    for (auto y = 0; y < height; ++y) {
      auto const *const row = reinterpret_cast<std::uint32_t const *>(
          static_cast<std::uint8_t const *>(data.pixel_data) + y * data.stride);
      for (auto x = 0; x < width; ++x) {
        auto const color = unpremultiply(row[x]);
        pixels.insert(pixels.end(), {color.r, color.g, color.b, color.a});
      }
    }
    return basalt::raster_image::create(static_cast<std::uint32_t>(width),
                                        static_cast<std::uint32_t>(height), std::move(pixels));
  }

private:
  platform *platform_;
  window_id window_;
  BLImage frame_;
  blend2d_painter painter_;
};

} // namespace

auto make_raster_backend(platform *platform, window_id window) -> std::unique_ptr<render_backend> {
  return std::make_unique<raster_backend>(platform, window);
}

} // namespace magma::render
