// An event loop that paces frames by the display. A bar moves across the window on every
// refresh, and a key press changes its color in the next frame without waiting for a refresh.

#include "magma/magma.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <variant>

namespace {

constexpr auto background = basalt::rgba{245, 245, 245, 255};
constexpr auto blue = basalt::rgba{40, 90, 200, 255};
constexpr auto orange = basalt::rgba{230, 120, 30, 255};

auto now_ns() -> std::int64_t { return magma::platform::event_clock_now().count(); }

// The display's refresh interval, for when the platform cannot follow the display itself.
auto refresh_interval(magma::platform const &platform, magma::window_id window) -> std::int64_t {
  auto const rate = platform.display_refresh_rate(window).value_or(60.0F);
  return static_cast<std::int64_t>(std::llround(1e9 / rate));
}

auto draw(basalt::size viewport, std::int64_t time, bool highlighted) -> basalt::paint_commands {
  auto const phase = static_cast<float>(time % 2'000'000'000) / 2e9F;
  auto const width = 40.0F;
  auto const left = phase * (viewport.width - width);
  return {basalt::fill_rect_command{{{0, 0}, viewport}, background},
          basalt::fill_rect_command{{{left, 0}, {width, viewport.height}},
                                    highlighted ? orange : blue}};
}

} // namespace

auto main() -> int {
  auto created = magma::platform::create();
  if (!created) {
    std::cerr << created.error().message << '\n';
    return EXIT_FAILURE;
  }
  auto platform = std::move(*created);
  auto const window = platform.create_window({
      .graphics = magma::preferred_graphics_api(magma::backend_preference::automatic),
      .title = "Magma event loop",
      .size = {640, 400},
  });
  if (!window) {
    std::cerr << window.error().message << '\n';
    return EXIT_FAILURE;
  }
  auto rendering = magma::renderer::create(platform, *window);
  if (!rendering) {
    std::cerr << rendering.error().message << '\n';
    return EXIT_FAILURE;
  }
  auto renderer = std::move(*rendering);

  auto scheduler = magma::frame_scheduler{refresh_interval(platform, *window)};
  auto highlighted = false;
  auto running = true;
  scheduler.request_frame();
  while (running) {
    // The display link stops after each tick, so ask for the next one while a frame is wanted.
    if (scheduler.pending()) {
      (void)platform.request_display_tick(*window);
    }
    auto const timeout = scheduler.wait_timeout(now_ns());
    platform.wait_for_events(timeout ? std::optional{std::chrono::nanoseconds{*timeout}}
                                     : std::nullopt);

    if (auto const tick = platform.take_display_tick(*window)) {
      scheduler.display_refreshed(tick->time.count(), tick->interval.count());
    }
    if (!renderer.frame_in_flight()) {
      scheduler.frame_shown();
    }
    for (auto const &incoming : platform.poll_events()) {
      if (auto const *const key = std::get_if<basalt::key_event>(&incoming.event)) {
        if (key->pressed && !key->repeat) {
          highlighted = !highlighted;
          scheduler.request_input_frame();
        }
      } else if (auto const *const event = std::get_if<basalt::window_event>(&incoming.event)) {
        switch (event->action) {
        case basalt::window_action::close_requested:
          running = false;
          break;
        case basalt::window_action::resized:
        case basalt::window_action::scale_changed:
        case basalt::window_action::shown:
          // The window may now be on another display.
          if (!scheduler.follows_display()) {
            scheduler.set_interval(refresh_interval(platform, *window));
          }
          renderer.request_full_redraw();
          scheduler.request_frame();
          break;
        default:
          break;
        }
      }
    }

    auto const frame_time = now_ns();
    if (running && scheduler.frame_due(frame_time)) {
      scheduler.begin_frame(frame_time);
      auto const viewport = platform.window_size(*window).value_or(basalt::size{});
      renderer.set_metrics(viewport, platform.display_scale(*window).value_or(1.0F));
      auto const whole = basalt::rect{{0, 0}, viewport};
      if (auto const drawn =
              renderer.render(draw(viewport, frame_time, highlighted), {&whole, 1}, background);
          !drawn) {
        std::cerr << drawn.error().message << '\n';
      }
      // The bar moves on every refresh.
      scheduler.request_frame();
    }
  }
  return EXIT_SUCCESS;
}
