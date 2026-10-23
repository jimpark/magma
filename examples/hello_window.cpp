// A Basalt widget tree in a Magma window. A button counts clicks, and a label shows the count.

#include "basalt/basalt.h"
#include "magma/magma.h"

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <format>
#include <iostream>
#include <optional>
#include <variant>

namespace {

auto now_ns() -> std::int64_t { return magma::platform::event_clock_now().count(); }

auto refresh_interval(magma::platform const &platform, magma::window_id window) -> std::int64_t {
  auto const rate = platform.display_refresh_rate(window).value_or(60.0F);
  return static_cast<std::int64_t>(std::llround(1e9 / rate));
}

auto is_input(basalt::event const &event) -> bool {
  return !std::holds_alternative<basalt::window_event>(event);
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
      .title = "Hello, Magma",
      .size = {480, 240},
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

  auto fonts = basalt::text::font_manager{};
  if (fonts.add_system_fonts() == 0) {
    std::cerr << "No system fonts found\n";
    return EXIT_FAILURE;
  }
  auto const shaper = basalt::text::text_shaper{fonts};

  auto const scheme =
      platform.system_dark_mode() ? basalt::color_scheme::dark : basalt::color_scheme::light;
  auto tree = basalt::widget_tree{basalt::make_theme(scheme)};
  tree.set_text_shaper(&shaper);
  tree.set_viewport(platform.window_size(*window).value_or(basalt::size{480, 240}));
  (void)tree.set_scale({.display = platform.display_scale(*window).value_or(1.0F)});

  auto clicks = 0;
  auto &root = tree.emplace_root<basalt::box>(basalt::axis::vertical);
  root.set_padding(basalt::length_insets::uniform(basalt::fixed(16)));
  root.set_gap(basalt::fixed(8));
  auto &status = root.emplace_child<basalt::label>("Clicks: 0");
  auto &press = root.emplace_child<basalt::button>("Count a click");
  press.set_variant(basalt::button_variant::primary);
  press.set_on_press([&] { status.set_text(std::format("Clicks: {}", ++clicks)); });
  auto router = basalt::event_router{tree};

  auto scheduler = magma::frame_scheduler{refresh_interval(platform, *window)};
  tree.set_frame_request_handler([&] {
    scheduler.request_frame();
    platform.wake();
  });

  auto const run_frame = [&] {
    auto const frame_time = now_ns();
    scheduler.begin_frame(frame_time);
    tree.advance_animations(std::chrono::nanoseconds{frame_time});
    tree.update_layout();
    router.update_hover();
    renderer.set_metrics(tree.viewport(), tree.scale().display);
    auto const damage = tree.take_damage();
    if (damage.empty() && !renderer.needs_full_redraw()) {
      return;
    }
    auto commands = basalt::paint_commands{};
    basalt::paint_tree(tree, commands);
    if (auto const drawn = renderer.render(commands, damage, tree.theme().colors.background_default);
        !drawn) {
      std::cerr << drawn.error().message << '\n';
    }
    tree.collect_garbage();
  };

  auto running = true;
  scheduler.request_frame();
  while (running) {
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
      if (auto const *const event = std::get_if<basalt::window_event>(&incoming.event)) {
        if (event->action == basalt::window_action::close_requested) {
          running = false;
        } else if (event->action == basalt::window_action::shown ||
                   event->action == basalt::window_action::resized ||
                   event->action == basalt::window_action::scale_changed) {
          if (!scheduler.follows_display()) {
            scheduler.set_interval(refresh_interval(platform, *window));
          }
          renderer.request_full_redraw();
          scheduler.request_frame();
        }
      }
      router.dispatch(incoming.event);
      if (is_input(incoming.event) && scheduler.pending()) {
        scheduler.request_input_frame();
      }
    }
    if (running && scheduler.frame_due(now_ns())) {
      run_frame();
    }
  }
  return EXIT_SUCCESS;
}
