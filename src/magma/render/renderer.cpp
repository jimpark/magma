#include "magma/renderer.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <utility>

#include "backend.h"

namespace magma {
namespace {

constexpr auto minimum_scale = 0.01F;

auto device_rect(basalt::rect area, float scale, int width, int height) -> pixel_rect {
  auto const left = std::clamp(static_cast<int>(std::floor(area.left() * scale)), 0, width);
  auto const top = std::clamp(static_cast<int>(std::floor(area.top() * scale)), 0, height);
  auto const right = std::clamp(static_cast<int>(std::ceil(area.right() * scale)), 0, width);
  auto const bottom = std::clamp(static_cast<int>(std::ceil(area.bottom() * scale)), 0, height);
  return {left, top, std::max(0, right - left), std::max(0, bottom - top)};
}

// Shared with the presentation handlers, which can run after the renderer is gone.
struct presentation_log {
  std::mutex mutex;
  bool tracking = false;
  std::vector<presentation> entries;
  // The last frame that reached the screen or was dropped by it.
  std::atomic<std::uint64_t> shown{0};

  auto add(presentation entry) -> void {
    auto previous = shown.load();
    while (previous < entry.frame && !shown.compare_exchange_weak(previous, entry.frame)) {
    }
    auto const lock = std::scoped_lock{mutex};
    if (tracking) {
      entries.push_back(entry);
    }
  }
};

} // namespace

namespace {

// A GPU backend for `preference`, or null when it asks for none or none can be created.
auto make_gpu_backend(backend_preference preference, platform *platform, window_id window)
    -> std::unique_ptr<render::render_backend> {
  if (preference == backend_preference::raster_only) {
    return nullptr;
  }
#if defined(MAGMA_HAS_METAL)
  if (auto created = render::make_metal_backend(platform, window)) {
    return std::move(*created);
  }
#endif
#if defined(MAGMA_HAS_D3D12)
  if (auto created = render::make_d3d12_backend(platform, window)) {
    return std::move(*created);
  }
#endif
#if defined(MAGMA_HAS_VULKAN)
  if (auto created = render::make_vulkan_backend(platform, window)) {
    return std::move(*created);
  }
#endif
  (void)platform;
  (void)window;
  return nullptr;
}

} // namespace

auto preferred_graphics_api(backend_preference preference) -> graphics_api {
#if defined(MAGMA_HAS_METAL)
  if (preference != backend_preference::raster_only) {
    return graphics_api::metal;
  }
#endif
  if (preference == backend_preference::raster_only) {
    return graphics_api::software;
  }
#if defined(MAGMA_HAS_D3D12)
  if (render::d3d12_device_available()) {
    return graphics_api::direct3d12;
  }
#endif
#if defined(MAGMA_HAS_VULKAN)
  if (render::vulkan_device_available()) {
    return graphics_api::vulkan;
  }
#endif
  return graphics_api::software;
}

struct renderer::impl {
  std::unique_ptr<render::render_backend> backend;
  basalt::size viewport{};
  float scale = 1;
  bool full_redraw = true;
  std::uint64_t frame = 0;
  // Kept between frames so that a frame does not allocate.
  std::vector<pixel_rect> areas;
  std::shared_ptr<presentation_log> presentations = std::make_shared<presentation_log>();

  [[nodiscard]] auto device_width() const -> int {
    return std::max(1, static_cast<int>(std::lround(viewport.width * scale)));
  }
  [[nodiscard]] auto device_height() const -> int {
    return std::max(1, static_cast<int>(std::lround(viewport.height * scale)));
  }
};

renderer::renderer(std::unique_ptr<impl> implementation) : impl_(std::move(implementation)) {}
renderer::renderer(renderer &&) noexcept = default;
auto renderer::operator=(renderer &&) noexcept -> renderer & = default;
renderer::~renderer() = default;

auto renderer::create(platform &platform, window_id window, backend_preference preference)
    -> std::expected<renderer, render_error> {
  auto state = std::make_unique<impl>();
  if (auto const size = platform.window_size(window); !size) {
    return std::unexpected(render_error{"unknown window"});
  } else {
    state->viewport = *size;
  }
  state->scale = std::max(platform.display_scale(window).value_or(1.0F), minimum_scale);
  state->backend = make_gpu_backend(preference, &platform, window);
  if (state->backend == nullptr) {
    state->backend = render::make_raster_backend(&platform, window);
  }
  return renderer{std::move(state)};
}

auto renderer::create_offscreen(backend_preference preference) -> renderer {
  auto state = std::make_unique<impl>();
  state->backend = make_gpu_backend(preference, nullptr, 0);
  if (state->backend == nullptr) {
    state->backend = render::make_raster_backend(nullptr, 0);
  }
  return renderer{std::move(state)};
}

auto renderer::active_backend() const -> backend { return impl_->backend->kind(); }

auto renderer::set_metrics(basalt::size viewport, float scale) -> void {
  scale = std::max(scale, minimum_scale);
  auto const previous_width = impl_->device_width();
  auto const previous_height = impl_->device_height();
  auto const previous_scale = impl_->scale;
  impl_->viewport = viewport;
  impl_->scale = scale;
  if (previous_width != impl_->device_width() || previous_height != impl_->device_height() ||
      previous_scale != scale) {
    impl_->full_redraw = true;
  }
}

auto renderer::viewport() const -> basalt::size { return impl_->viewport; }
auto renderer::scale() const -> float { return impl_->scale; }
auto renderer::pixel_size() const -> basalt::size {
  return {static_cast<float>(impl_->device_width()), static_cast<float>(impl_->device_height())};
}
auto renderer::needs_full_redraw() const -> bool { return impl_->full_redraw; }
auto renderer::request_full_redraw() -> void { impl_->full_redraw = true; }

auto renderer::render(basalt::paint_commands const &commands, std::span<basalt::rect const> damage,
                      basalt::rgba clear) -> std::expected<void, render_error> {
  if (impl_->viewport.empty()) {
    return {};
  }
  if (!impl_->full_redraw && damage.empty()) {
    return {};
  }
  auto const width = impl_->device_width();
  auto const height = impl_->device_height();

  auto &areas = impl_->areas;
  areas.clear();
  if (impl_->full_redraw) {
    areas.push_back({0, 0, width, height});
  } else {
    for (auto const &area : damage) {
      auto const device = device_rect(area, impl_->scale, width, height);
      if (device.width > 0 && device.height > 0) {
        areas.push_back(device);
      }
    }
    if (areas.empty()) {
      return {};
    }
  }

  auto const frame = impl_->frame + 1;
  auto const log = impl_->presentations;
  auto const reported = impl_->backend->report_presentation(
      [log, frame](std::optional<std::chrono::nanoseconds> time) { log->add({frame, time}); });
  auto result = impl_->backend->draw({.commands = commands,
                                      .areas = areas,
                                      .full_redraw = impl_->full_redraw,
                                      .width = width,
                                      .height = height,
                                      .scale = impl_->scale,
                                      .clear = clear});
  if (result) {
    impl_->full_redraw = !impl_->backend->retains_frame();
    impl_->frame = frame;
    if (!reported) {
      log->add({frame, platform::event_clock_now()});
    }
  }
  return result;
}

auto renderer::read_pixel(int x, int y) const -> std::optional<basalt::rgba> {
  return impl_->backend->read_pixel(x, y);
}

auto renderer::read_frame() const -> basalt::raster_image_ptr {
  return impl_->backend->read_frame();
}

auto renderer::track_presentations(bool enabled) -> void {
  auto const lock = std::scoped_lock{impl_->presentations->mutex};
  impl_->presentations->tracking = enabled;
  if (!enabled) {
    impl_->presentations->entries.clear();
  }
}

auto renderer::frame_in_flight() const -> bool {
  return impl_->presentations->shown.load() < impl_->frame;
}

auto renderer::last_frame() const -> std::uint64_t { return impl_->frame; }

auto renderer::take_presentations() -> std::vector<presentation> {
  auto const lock = std::scoped_lock{impl_->presentations->mutex};
  return std::exchange(impl_->presentations->entries, {});
}

} // namespace magma
