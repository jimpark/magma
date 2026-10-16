#include "magma/renderer.h"

#include <algorithm>
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
  std::vector<presentation> entries;

  auto add(presentation entry) -> void {
    auto const lock = std::scoped_lock{mutex};
    entries.push_back(entry);
  }
};

} // namespace

auto preferred_graphics_api(backend_preference preference) -> graphics_api {
  (void)preference;
  return graphics_api::software;
}

struct renderer::impl {
  std::unique_ptr<render::render_backend> backend;
  basalt::size viewport{};
  float scale = 1;
  bool full_redraw = true;
  std::uint64_t frame = 0;
  std::shared_ptr<presentation_log> presentations;

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
  (void)preference;
  auto state = std::make_unique<impl>();
  if (auto const size = platform.window_size(window); !size) {
    return std::unexpected(render_error{"unknown window"});
  } else {
    state->viewport = *size;
  }
  state->scale = std::max(platform.display_scale(window).value_or(1.0F), minimum_scale);
  state->backend = render::make_raster_backend(&platform, window);
  return renderer{std::move(state)};
}

auto renderer::create_offscreen() -> renderer {
  auto state = std::make_unique<impl>();
  state->backend = render::make_raster_backend(nullptr, 0);
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

  auto areas = std::vector<pixel_rect>{};
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
  auto const reported =
      log != nullptr &&
      impl_->backend->report_presentation(
          [log, frame](std::optional<std::chrono::nanoseconds> time) { log->add({frame, time}); });
  auto result = impl_->backend->draw({.commands = commands,
                                      .areas = areas,
                                      .full_redraw = impl_->full_redraw,
                                      .width = width,
                                      .height = height,
                                      .scale = impl_->scale,
                                      .clear = clear});
  if (result) {
    impl_->full_redraw = false;
    impl_->frame = frame;
    if (log != nullptr && !reported) {
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
  if (!enabled) {
    impl_->presentations.reset();
  } else if (impl_->presentations == nullptr) {
    impl_->presentations = std::make_shared<presentation_log>();
  }
}

auto renderer::last_frame() const -> std::uint64_t { return impl_->frame; }

auto renderer::take_presentations() -> std::vector<presentation> {
  if (impl_->presentations == nullptr) {
    return {};
  }
  auto const lock = std::scoped_lock{impl_->presentations->mutex};
  return std::exchange(impl_->presentations->entries, {});
}

} // namespace magma
