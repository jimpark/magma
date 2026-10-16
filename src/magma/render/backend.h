#pragma once

#include "basalt/color.h"
#include "basalt/image.h"
#include "basalt/paint.h"

#include "magma/platform.h"
#include "magma/renderer.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>

namespace magma::render {

using presented_handler = std::function<void(std::optional<std::chrono::nanoseconds>)>;

// One frame for a backend. `areas` are in device pixels, inside the frame. A full redraw has one
// area that covers the whole frame.
struct frame_request {
  basalt::paint_commands const &commands;
  std::span<pixel_rect const> areas;
  bool full_redraw = false;
  int width = 0;
  int height = 0;
  float scale = 1;
  basalt::rgba clear;
};

// Draws frames and moves them to the screen. A backend keeps its frame between calls, so a
// partial redraw changes only the requested areas.
class render_backend {
public:
  virtual ~render_backend() = default;

  [[nodiscard]] virtual auto kind() const -> backend = 0;
  // False when the backend draws each frame from nothing, so every frame must be a full redraw.
  [[nodiscard]] virtual auto retains_frame() const -> bool { return true; }
  // Draws and presents one frame. A frame of a new size starts with undefined contents, and
  // the caller then sends a full redraw.
  [[nodiscard]] virtual auto draw(frame_request const &request)
      -> std::expected<void, render_error> = 0;
  // Asks the backend to call `handler` once when the next presented frame reaches the screen,
  // with its time on the platform's event clock, or with nothing when it is never shown. The
  // call can come from any thread. Returns false when the backend cannot tell, and then the
  // caller records the time `draw` returns.
  [[nodiscard]] virtual auto report_presentation(presented_handler handler) -> bool {
    (void)handler;
    return false;
  }
  [[nodiscard]] virtual auto read_pixel(int x, int y) const -> std::optional<basalt::rgba> = 0;
  [[nodiscard]] virtual auto read_frame() const -> basalt::raster_image_ptr = 0;
};

// The color of a premultiplied 32-bit BGRA pixel, as Blend2D and the GPU backends store frames.
[[nodiscard]] inline auto unpremultiply(std::uint32_t pixel) -> basalt::rgba {
  auto const alpha = pixel >> 24;
  auto const channel = [alpha](std::uint32_t value) {
    return static_cast<std::uint8_t>(alpha == 0 ? 0 : (value * 255 + alpha / 2) / alpha);
  };
  return {channel(pixel >> 16 & 0xff), channel(pixel >> 8 & 0xff), channel(pixel & 0xff),
          static_cast<std::uint8_t>(alpha)};
}

// Draws with Blend2D. With a platform it presents through `present_pixels`; without one it
// keeps the frame in memory only.
[[nodiscard]] auto make_raster_backend(platform *platform, window_id window)
    -> std::unique_ptr<render_backend>;

#if defined(MAGMA_HAS_METAL)
// Draws with Metal. With a platform it presents in the window's CAMetalLayer, and the window must
// have been created for Metal; without one it keeps the frame in a texture only.
[[nodiscard]] auto make_metal_backend(platform *platform, window_id window)
    -> std::expected<std::unique_ptr<render_backend>, render_error>;
// True when the computer has a Metal device, so that the Metal backend must start.
[[nodiscard]] auto metal_device_available() -> bool;
#endif

#if defined(MAGMA_HAS_VULKAN)
// Draws with Vulkan. With a platform it presents in a surface of the window, which must have been
// created with `graphics_api::vulkan`; without one it keeps the frame in an image only.
[[nodiscard]] auto make_vulkan_backend(platform *platform, window_id window)
    -> std::expected<std::unique_ptr<render_backend>, render_error>;
// True when the computer has a Vulkan device that the backend accepts, so that the Vulkan
// backend must start. A software device counts only when MAGMA_VULKAN_ALLOW_SOFTWARE is 1.
[[nodiscard]] auto vulkan_device_available() -> bool;
#endif

#if defined(MAGMA_HAS_D3D12)
// Draws with Direct3D 12. With a platform it presents in a swap chain of the window; without one
// it keeps the frame in a texture only.
[[nodiscard]] auto make_d3d12_backend(platform *platform, window_id window)
    -> std::expected<std::unique_ptr<render_backend>, render_error>;
// True when the computer has a Direct3D 12 device that the backend accepts, so that the Direct3D 12
// backend must start. WARP counts only when MAGMA_D3D12_ADAPTER is `warp`.
[[nodiscard]] auto d3d12_device_available() -> bool;
#endif

} // namespace magma::render
