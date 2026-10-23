# Getting Started

This chapter builds a small program in seven steps. The program opens a window with a label and a
button. Each click on the button increases the count in the label. The program is the same as
`examples/hello_window.cpp`.

## Step 1: Add Magma to your project

Add Magma with CMake `FetchContent`. Use a release tag as `GIT_TAG`. The target `magma::magma`
brings `basalt::basalt` with it.

```cmake
cmake_minimum_required(VERSION 3.25)
project(hello LANGUAGES CXX)

include(FetchContent)
FetchContent_Declare(magma
  GIT_REPOSITORY https://github.com/nadiasvertex/magma.git
  GIT_TAG v0.6.0)
FetchContent_MakeAvailable(magma)

add_executable(hello main.cpp)
target_link_libraries(hello PRIVATE magma::magma)
```

Magma can also be used with `add_subdirectory` or `find_package`. See `README.md` for both. The
build downloads Basalt, SDL 3, and Blend2D at configure time. On Linux, install the X11 and
Wayland development packages first. `README.md` lists them.

## Step 2: Create the platform and the window

Create one `platform`. Then create a window with the graphics API that `preferred_graphics_api`
gives. The renderer can use the GPU only with that graphics API.

```cpp
#include "basalt/basalt.h"
#include "magma/magma.h"

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
```

The window size is in layout pixels. On a display with a scale of 2, the window has 960 by 480
device pixels.

## Step 3: Create the renderer

Create a renderer for the window. The platform must stay alive while the renderer exists.

```cpp
  auto rendering = magma::renderer::create(platform, *window);
  if (!rendering) {
    std::cerr << rendering.error().message << '\n';
    return EXIT_FAILURE;
  }
  auto renderer = std::move(*rendering);
```

The default preference is `backend_preference::automatic`. The renderer uses the GPU when it can,
and the CPU when it cannot. `renderer.active_backend()` tells which backend it uses.

## Step 4: Create the widget tree

Load fonts, make a Basalt widget tree, and add widgets. Use the window's size and display scale.
`system_dark_mode` tells which color scheme the operating system uses.

```cpp
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
```

The router keeps the viewport and the scale up to date after this step. It reads them from the
`resized` and `scale_changed` window events that the platform sends.

## Step 5: Connect the scheduler to the tree

Make a `frame_scheduler` with the display's refresh interval. When the tree needs a frame, request
one from the scheduler and end the wait for events.

```cpp
  auto scheduler = magma::frame_scheduler{refresh_interval(platform, *window)};
  tree.set_frame_request_handler([&] {
    scheduler.request_frame();
    platform.wake();
  });
```

`refresh_interval` is a small function in the example. It converts `display_refresh_rate` to
nanoseconds, and uses 60 Hz when the system does not report a rate.

## Step 6: Draw a frame

Put the frame in a function. It runs Basalt's frame cycle and gives the result to the renderer.

```cpp
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
```

`set_metrics` tells the renderer the viewport and the scale. A change of size or scale discards
the frame and sets `needs_full_redraw`. Then `render` ignores the damage and draws the whole
viewport. This is why the function paints the whole tree.

`now_ns` gives the time on the platform's event clock, from `platform::event_clock_now`. Use the
same clock for the scheduler, the display ticks, and the events.

## Step 7: Run the event loop

The loop waits for events, updates the scheduler, sends events to the tree, and draws frames.

```cpp
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
```

These are the important points:

- Ask for a display tick only while a frame is pending. An idle application gets no ticks and
  uses no processor time.
- When the window is shown, resized, or moved to another display, request a full redraw. The window
  can be on a display with a different refresh rate.
- When an input event causes a frame, call `request_input_frame`. Then the frame starts at once
  if the previous frame is on the screen.

Build and run the program. Click the button. The label shows the number of clicks.

## Where to go next

- Read [Architecture](architecture.md) for the rules of each part.
- Read [Frame Scheduling](frame-scheduling.md) to learn when frames start.
- Read [Rendering](rendering.md) to select a backend or render without a window.
- Open `examples/event_loop.cpp` for a loop that draws an animation on every refresh.
- Read `spec/CONVENTIONS.md` before you change the library.
