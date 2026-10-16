#pragma once

#include "basalt/color.h"
#include "basalt/geometry.h"
#include "basalt/image.h"
#include "basalt/paint.h"

#include "magma/platform.h"

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace magma {

// When a frame reached the screen. `time` is on the platform's event clock and is empty when
// the frame was never shown, for example because a later frame replaced it.
struct presentation {
  std::uint64_t frame = 0;
  std::optional<std::chrono::nanoseconds> time;
};

struct render_error {
  std::string message;
};

enum class backend {
  raster, // Blend2D on the CPU, presented with `platform::present_pixels`.
  metal,  // Metal on the GPU (macOS only).
  vulkan, // Vulkan on the GPU (Linux and Windows).
  d3d12,  // Direct3D 12 on the GPU (Windows only).
};

enum class backend_preference {
  automatic,   // A GPU backend when one can be created, otherwise raster.
  raster_only, // Never create a GPU device.
};

// The window graphics API that `backend_preference` needs on this platform. Create the window
// with it so that the renderer can use the GPU.
[[nodiscard]] auto preferred_graphics_api(backend_preference preference) -> graphics_api;

// Replays Basalt paint commands and presents the result in a window.
//
// Commands are in layout pixels. The renderer keeps a frame of `viewport * scale` device pixels,
// so a frame redraws only the damaged rects and presents the rest unchanged.
//
// The Metal backend can instead draw each frame from nothing into the window. Set the environment
// variable MAGMA_METAL_REDRAW to `full` before the renderer is created to use it. Then
// `needs_full_redraw` is always true, and `read_pixel` and `read_frame` return nothing for a
// window. The default, `retained`, keeps the frame as described above.
class renderer {
public:
  renderer(renderer const &) = delete;
  auto operator=(renderer const &) -> renderer & = delete;
  renderer(renderer &&) noexcept;
  auto operator=(renderer &&) noexcept -> renderer &;
  ~renderer();

  // Renders into `window`, with the window's size and display scale as the first metrics. The
  // platform must outlive the renderer.
  [[nodiscard]] static auto create(platform &platform, window_id window,
                                   backend_preference preference = backend_preference::automatic)
      -> std::expected<renderer, render_error>;

  // Renders into memory only, for tests and tools. Set the metrics before the first frame. The
  // default draws on the CPU, which gives the reference image; `automatic` uses the GPU when it
  // can.
  [[nodiscard]] static auto
  create_offscreen(backend_preference preference = backend_preference::raster_only) -> renderer;

  [[nodiscard]] auto active_backend() const -> backend;

  // Sets the viewport in layout pixels and the layout-to-device scale. A change of device size
  // or scale discards the frame and requests a full redraw.
  auto set_metrics(basalt::size viewport, float scale) -> void;
  [[nodiscard]] auto viewport() const -> basalt::size;
  [[nodiscard]] auto scale() const -> float;
  // The frame size in device pixels.
  [[nodiscard]] auto pixel_size() const -> basalt::size;

  // True until the next frame that repaints everything. While it is set, `render` ignores the
  // damage and repaints the whole viewport, so the commands must cover all of it.
  [[nodiscard]] auto needs_full_redraw() const -> bool;
  // Repaints the whole frame on the next render, for example when the window was exposed.
  auto request_full_redraw() -> void;

  // Draws `commands` clipped to `damage` (layout pixels), after clearing the damaged area to
  // `clear`, then presents the frame. Empty damage with no pending full redraw presents nothing.
  [[nodiscard]] auto render(basalt::paint_commands const &commands,
                            std::span<basalt::rect const> damage, basalt::rgba clear)
      -> std::expected<void, render_error>;

  // The color of one device pixel in the last rendered frame, if it exists.
  [[nodiscard]] auto read_pixel(int x, int y) const -> std::optional<basalt::rgba>;
  // A copy of the last rendered frame in device pixels, or null before the first frame.
  [[nodiscard]] auto read_frame() const -> basalt::raster_image_ptr;

  // Records when frames reach the screen, for frame statistics. Off by default.
  auto track_presentations(bool enabled) -> void;
  // The number of the last frame `render` presented, counting from 1; 0 before the first.
  [[nodiscard]] auto last_frame() const -> std::uint64_t;
  // True while the last presented frame has not reached the screen. Then a new frame would wait
  // for it, so a frame for input is better started at the next display refresh. The raster
  // backend hands each frame to the window system at once, so it is never in flight.
  [[nodiscard]] auto frame_in_flight() const -> bool;
  // The presentations recorded since the last call, in the order they were reported. The raster
  // backend reports when the frame was handed to the window system.
  [[nodiscard]] auto take_presentations() -> std::vector<presentation>;

private:
  struct impl;
  explicit renderer(std::unique_ptr<impl> implementation);
  std::unique_ptr<impl> impl_;
};

} // namespace magma
