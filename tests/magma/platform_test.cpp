#include <catch2/catch_test_macros.hpp>

#include "basalt/event.h"

#include "magma/platform.h"
#include "magma/platform/sdl_platform_internal.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

// These tests use SDL's dummy video driver, so they need no display. They push SDL events
// directly and check what `poll_events` makes of them.

using namespace std::chrono_literals;

namespace {

auto make_platform() -> magma::platform {
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
  auto created = magma::platform::create();
  REQUIRE(created.has_value());
  return std::move(*created);
}

auto make_window(magma::platform &platform, basalt::size size) -> magma::window_id {
  auto config = magma::window_config{};
  config.size = size;
  auto const window = platform.create_window(config);
  REQUIRE(window.has_value());
  // Creating a window queues events of its own, such as `shown`.
  (void)platform.poll_events();
  return *window;
}

auto push(SDL_Event event) -> void { REQUIRE(SDL_PushEvent(&event)); }

template <class T>
auto first_of(std::vector<magma::platform_event> const &events) -> std::optional<T> {
  for (auto const &entry : events) {
    if (auto const *const value = std::get_if<T>(&entry.event)) {
      return *value;
    }
  }
  return std::nullopt;
}

struct pixel {
  std::uint8_t red = 0;
  std::uint8_t green = 0;
  std::uint8_t blue = 0;
};

auto window_pixel(magma::window_id window, int x, int y) -> pixel {
  auto *const surface = SDL_GetWindowSurface(SDL_GetWindowFromID(window));
  REQUIRE(surface != nullptr);
  auto result = pixel{};
  auto alpha = std::uint8_t{};
  REQUIRE(SDL_ReadSurfacePixel(surface, x, y, &result.red, &result.green, &result.blue, &alpha));
  return result;
}

// Premultiplied BGRA, opaque, in one color.
auto solid_pixels(int width, int height, pixel color) -> std::vector<std::byte> {
  auto bytes = std::vector<std::byte>{};
  bytes.reserve(static_cast<std::size_t>(width * height * 4));
  for (int i = 0; i < width * height; ++i) {
    bytes.push_back(std::byte{color.blue});
    bytes.push_back(std::byte{color.green});
    bytes.push_back(std::byte{color.red});
    bytes.push_back(std::byte{0xff});
  }
  return bytes;
}

} // namespace

TEST_CASE("a window reports the size it was created with", "[platform]") {
  auto platform = make_platform();
  auto const window = make_window(platform, {320, 200});
  CHECK(platform.window_size(window) == basalt::size{320, 200});
  CHECK(platform.pixel_size(window) == basalt::size{320, 200});
  CHECK(platform.display_scale(window) == 1.0f);
}

TEST_CASE("an unknown window has no size and cannot be presented", "[platform]") {
  auto platform = make_platform();
  auto const unknown = magma::window_id{9999};
  CHECK_FALSE(platform.window_size(unknown).has_value());
  CHECK_FALSE(platform.pixel_size(unknown).has_value());
  CHECK_FALSE(platform.display_refresh_rate(unknown).has_value());
  auto const pixels = solid_pixels(1, 1, {});
  CHECK_FALSE(platform.present_pixels(unknown, pixels, 1, 1, 4, {}).has_value());
}

TEST_CASE("presented pixels reach the window surface", "[platform]") {
  auto platform = make_platform();
  auto const window = make_window(platform, {8, 4});
  auto const pixels = solid_pixels(8, 4, {0x33, 0x22, 0x11});
  REQUIRE(platform.present_pixels(window, pixels, 8, 4, 8 * 4, {}).has_value());
  auto const shown = window_pixel(window, 5, 3);
  CHECK(shown.red == 0x33);
  CHECK(shown.green == 0x22);
  CHECK(shown.blue == 0x11);
}

TEST_CASE("only the damaged area of the pixels is presented", "[platform]") {
  auto platform = make_platform();
  auto const window = make_window(platform, {8, 4});
  auto const first = solid_pixels(8, 4, {0xff, 0, 0});
  REQUIRE(platform.present_pixels(window, first, 8, 4, 8 * 4, {}).has_value());

  auto const second = solid_pixels(8, 4, {0, 0xff, 0});
  auto const damage = std::array{magma::pixel_rect{0, 0, 2, 2}};
  REQUIRE(platform.present_pixels(window, second, 8, 4, 8 * 4, damage).has_value());
  CHECK(window_pixel(window, 1, 1).green == 0xff);
  CHECK(window_pixel(window, 5, 3).red == 0xff);
}

TEST_CASE("a pixel buffer smaller than its dimensions is refused", "[platform]") {
  auto platform = make_platform();
  auto const window = make_window(platform, {8, 4});
  auto const pixels = solid_pixels(8, 3, {});
  CHECK_FALSE(platform.present_pixels(window, pixels, 8, 4, 8 * 4, {}).has_value());
}

TEST_CASE("key events become Basalt keys with their modifiers and time", "[platform][events]") {
  auto platform = make_platform();
  auto const window = make_window(platform, {100, 100});
  auto event = SDL_Event{};
  event.key.type = SDL_EVENT_KEY_DOWN;
  event.key.timestamp = 1'234'567;
  event.key.windowID = window;
  event.key.key = SDLK_A;
  event.key.mod = SDL_KMOD_LSHIFT | SDL_KMOD_LGUI;
  event.key.down = true;
  push(event);

  auto const events = platform.poll_events();
  auto const key = first_of<basalt::key_event>(events);
  REQUIRE(key.has_value());
  CHECK(key->code == basalt::key::a);
  CHECK(key->pressed);
  CHECK(key->modifiers == (basalt::modifier::shift | basalt::modifier::meta));
  CHECK(key->time == 1'234'567ns);
}

TEST_CASE("events carry the window they belong to, and quit uses window 0", "[platform][events]") {
  auto platform = make_platform();
  auto const window = make_window(platform, {100, 100});

  auto text = SDL_Event{};
  text.text.type = SDL_EVENT_TEXT_INPUT;
  text.text.windowID = window;
  text.text.text = "é";
  push(text);
  auto quit = SDL_Event{};
  quit.type = SDL_EVENT_QUIT;
  push(quit);

  auto const events = platform.poll_events();
  REQUIRE(events.size() == 2);
  CHECK(events[0].window == window);
  CHECK(std::get<basalt::text_input_event>(events[0].event).text == "é");
  CHECK(events[1].window == 0);
  CHECK(std::get<basalt::window_event>(events[1].event).action ==
        basalt::window_action::close_requested);
}

TEST_CASE("a close request reaches the application and leaves the window open",
          "[platform][events]") {
  auto platform = make_platform();
  auto const window = make_window(platform, {100, 100});

  auto close = SDL_Event{};
  close.window.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
  close.window.windowID = window;
  push(close);

  auto const events = platform.poll_events();
  REQUIRE(events.size() == 1);
  CHECK(events[0].window == window);
  CHECK(std::get<basalt::window_event>(events[0].event).action ==
        basalt::window_action::close_requested);
  CHECK(platform.window_size(window).has_value());
}

TEST_CASE("a live resize callback sees each resize and can be removed", "[platform][events]") {
  auto platform = make_platform();
  auto const window = make_window(platform, {100, 100});
  auto seen = std::vector<magma::platform_event>{};
  platform.set_live_resize_callback(
      [&seen](magma::platform_event const &event) { seen.push_back(event); });

  auto resized = SDL_Event{};
  resized.window.type = SDL_EVENT_WINDOW_RESIZED;
  resized.window.windowID = window;
  push(resized);
  REQUIRE(seen.size() == 1);
  CHECK(seen[0].window == window);
  CHECK(std::get<basalt::window_event>(seen[0].event).action == basalt::window_action::resized);
  CHECK(platform.poll_events().size() == 1);

  platform.set_live_resize_callback({});
  push(resized);
  CHECK(seen.size() == 1);
}

TEST_CASE("a wheel event scrolls the way the user's setting reports it", "[platform][events]") {
  auto platform = make_platform();
  auto const window = make_window(platform, {100, 100});
  auto event = SDL_Event{};
  event.wheel.type = SDL_EVENT_MOUSE_WHEEL;
  event.wheel.windowID = window;
  // SDL: one notch toward the top of the content.
  event.wheel.y = 1;
  event.wheel.mouse_x = 10;
  event.wheel.mouse_y = 20;
  event.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
  push(event);
  // A reversed setting has already reversed y; FLIPPED only reports that.
  event.wheel.direction = SDL_MOUSEWHEEL_FLIPPED;
  push(event);
  event.wheel.y = 0;
  event.wheel.x = 1;
  push(event);

  auto wheels = std::vector<basalt::wheel_event>{};
  for (auto const &entry : platform.poll_events()) {
    if (auto const *const wheel = std::get_if<basalt::wheel_event>(&entry.event)) {
      wheels.push_back(*wheel);
    }
  }
  REQUIRE(wheels.size() == 3);
  CHECK(wheels[0].position == basalt::point{10, 20});
  // Basalt: negative y moves the content down, which shows what is above.
  CHECK(wheels[0].delta == basalt::point{0, -40});
  CHECK(wheels[0].precise);
  CHECK(wheels[1].delta == basalt::point{0, -40});
  CHECK(wheels[2].delta == basalt::point{40, 0});
}

namespace {

auto drop_event(SDL_EventType type, magma::window_id window, float x, float y,
                char const *data = nullptr) -> SDL_Event {
  auto event = SDL_Event{};
  event.drop.type = type;
  event.drop.windowID = window;
  event.drop.x = x;
  event.drop.y = y;
  event.drop.data = data;
  return event;
}

auto file_drops(magma::platform &platform) -> std::vector<basalt::file_drop_event> {
  auto drops = std::vector<basalt::file_drop_event>{};
  for (auto const &entry : platform.poll_events()) {
    if (auto const *const drop = std::get_if<basalt::file_drop_event>(&entry.event)) {
      drops.push_back(*drop);
    }
  }
  return drops;
}

} // namespace

TEST_CASE("files dropped on a window become one drop with every path", "[platform][events]") {
  auto platform = make_platform();
  auto const window = make_window(platform, {100, 100});
  push(drop_event(SDL_EVENT_DROP_BEGIN, window, 0, 0));
  push(drop_event(SDL_EVENT_DROP_POSITION, window, 10, 20));
  push(drop_event(SDL_EVENT_DROP_POSITION, window, 30, 40));
  push(drop_event(SDL_EVENT_DROP_FILE, window, 30, 40, "/photos/a.jpg"));
  push(drop_event(SDL_EVENT_DROP_FILE, window, 30, 40, "/photos/b c.jpg"));
  push(drop_event(SDL_EVENT_DROP_COMPLETE, window, 30, 40));

  auto const drops = file_drops(platform);
  REQUIRE(drops.size() == 3);
  CHECK(drops[0].action == basalt::file_drop_action::enter);
  CHECK(drops[0].position == basalt::point{10, 20});
  CHECK(drops[1].action == basalt::file_drop_action::move);
  CHECK(drops[1].position == basalt::point{30, 40});
  CHECK(drops[2].action == basalt::file_drop_action::drop);
  CHECK(drops[2].position == basalt::point{30, 40});
  CHECK(drops[2].paths == std::vector<std::string>{"/photos/a.jpg", "/photos/b c.jpg"});
}

TEST_CASE("a file drag that leaves the window ends with a leave", "[platform][events]") {
  auto platform = make_platform();
  auto const window = make_window(platform, {100, 100});
  push(drop_event(SDL_EVENT_DROP_BEGIN, window, 0, 0));
  push(drop_event(SDL_EVENT_DROP_POSITION, window, 10, 20));
  push(drop_event(SDL_EVENT_DROP_COMPLETE, window, 0, 0));

  auto const drops = file_drops(platform);
  REQUIRE(drops.size() == 2);
  CHECK(drops[0].action == basalt::file_drop_action::enter);
  CHECK(drops[1].action == basalt::file_drop_action::leave);
  CHECK(drops[1].paths.empty());

  // The next drag starts over with an enter.
  push(drop_event(SDL_EVENT_DROP_BEGIN, window, 0, 0));
  push(drop_event(SDL_EVENT_DROP_POSITION, window, 5, 5));
  auto const next = file_drops(platform);
  REQUIRE(next.size() == 1);
  CHECK(next[0].action == basalt::file_drop_action::enter);
}

TEST_CASE("a button press counts the pressed button as held", "[platform][events]") {
  auto platform = make_platform();
  auto const window = make_window(platform, {100, 100});
  auto event = SDL_Event{};
  event.button.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  event.button.windowID = window;
  event.button.button = SDL_BUTTON_LEFT;
  event.button.down = true;
  event.button.clicks = 2;
  event.button.x = 3;
  event.button.y = 4;
  push(event);

  auto const pointer = first_of<basalt::pointer_event>(platform.poll_events());
  REQUIRE(pointer.has_value());
  CHECK(pointer->action == basalt::pointer_action::down);
  CHECK(pointer->button == basalt::pointer_button::primary);
  CHECK(basalt::has(pointer->buttons, basalt::pointer_button::primary));
  CHECK(pointer->click_count == 2);
  CHECK(pointer->position == basalt::point{3, 4});
}

TEST_CASE("input method selections are measured in UTF-8 bytes", "[platform][events]") {
  auto platform = make_platform();
  auto const window = make_window(platform, {100, 100});
  auto event = SDL_Event{};
  event.edit.type = SDL_EVENT_TEXT_EDITING;
  event.edit.windowID = window;
  event.edit.text = "äbc";
  event.edit.start = 1;
  event.edit.length = 1;
  push(event);

  auto const ime = first_of<basalt::ime_event>(platform.poll_events());
  REQUIRE(ime.has_value());
  CHECK(ime->text == "äbc");
  CHECK(ime->selection_start == 2);
  CHECK(ime->selection_length == 1);
}

TEST_CASE("event times and event_clock_now use the same clock", "[platform][events]") {
  auto platform = make_platform();
  auto const window = make_window(platform, {100, 100});
  auto const before = magma::platform::event_clock_now();
  auto event = SDL_Event{};
  event.text.type = SDL_EVENT_TEXT_INPUT;
  event.text.windowID = window;
  event.text.text = "a";
  push(event);
  auto const events = platform.poll_events();
  auto const after = magma::platform::event_clock_now();

  auto const text = first_of<basalt::text_input_event>(events);
  REQUIRE(text.has_value());
  CHECK(text->time >= before);
  CHECK(text->time <= after);
}

TEST_CASE("wake ends a wait without producing an event", "[platform][events]") {
  auto platform = make_platform();
  (void)platform.poll_events();
  platform.wake();
  auto const start = std::chrono::steady_clock::now();
  platform.wait_for_events(10s);
  CHECK(std::chrono::steady_clock::now() - start < 5s);
  CHECK(platform.poll_events().empty());
}

TEST_CASE("a timeout with a fraction of a millisecond is waited in full", "[platform][events]") {
  auto platform = make_platform();
  for (auto i = 0; i < 5; ++i) {
    (void)platform.poll_events();
    auto const start = magma::platform::event_clock_now();
    platform.wait_for_events(2900us);
    auto const waited = magma::platform::event_clock_now() - start;
    CHECK(waited >= 2900us);
    CHECK(waited < 5s);
  }
}

TEST_CASE("display ticks need a window on a display the platform can follow", "[platform]") {
  auto platform = make_platform();
  CHECK_FALSE(platform.request_display_tick(9999));
  CHECK_FALSE(platform.take_display_tick(9999).has_value());

  // The dummy driver has no display link, so the caller falls back to a timer.
  auto const window = make_window(platform, {40, 30});
  CHECK_FALSE(platform.request_display_tick(window));
  CHECK_FALSE(platform.take_display_tick(window).has_value());
}

// A real dialog needs a person to answer it, so these tests give the dialog callback the lists that
// SDL gives it. Folder, file and save dialogs share this path.
TEST_CASE("a dialog callback receives the chosen paths", "[platform][dialog]") {
  auto received = std::optional<std::vector<std::string>>{};
  auto called = 0;
  auto errors = std::make_shared<std::vector<std::exception_ptr>>();
  auto state = magma::detail::make_dialog_state(
      {},
      [&](std::optional<std::vector<std::string>> paths) {
        received = std::move(paths);
        ++called;
      },
      {}, errors);
  auto const chosen = std::array<char const *, 3>{"/photos/2024", "/photos/2025", nullptr};
  magma::detail::on_dialog(state.release(), chosen.data(), 0);
  REQUIRE(called == 1);
  REQUIRE(received.has_value());
  CHECK(*received == std::vector<std::string>{"/photos/2024", "/photos/2025"});
  CHECK(errors->empty());
}

TEST_CASE("a cancelled dialog gives the callback no paths", "[platform][dialog]") {
  auto called = 0;
  auto received = std::optional<std::vector<std::string>>{std::vector<std::string>{"stale"}};
  auto state = magma::detail::make_dialog_state(
      {},
      [&](std::optional<std::vector<std::string>> paths) {
        received = std::move(paths);
        ++called;
      },
      {}, std::make_shared<std::vector<std::exception_ptr>>());
  magma::detail::on_dialog(state.release(), nullptr, 0);
  CHECK(called == 1);
  CHECK_FALSE(received.has_value());
}

TEST_CASE("an exception from a dialog callback is kept for poll_events", "[platform][dialog]") {
  auto errors = std::make_shared<std::vector<std::exception_ptr>>();
  auto state = magma::detail::make_dialog_state(
      {}, [](std::optional<std::vector<std::string>>) { throw std::runtime_error("handler"); }, {},
      errors);
  auto const chosen = std::array<char const *, 2>{"/photos", nullptr};
  REQUIRE_NOTHROW(magma::detail::on_dialog(state.release(), chosen.data(), 0));
  REQUIRE(errors->size() == 1);
  // CHECK_THROWS_AS adds code after the [[noreturn]] rethrow, which MSVC reports as C4702.
  auto caught = false;
  try {
    std::rethrow_exception(errors->front());
  } catch (std::runtime_error const &) {
    caught = true;
  }
  CHECK(caught);
}

TEST_CASE("a dialog keeps its starting folder and filters until SDL calls back",
          "[platform][dialog]") {
  auto const filters = std::array{magma::file_filter{"Images", "jpg;png"}};
  auto state = magma::detail::make_dialog_state(
      filters, [](std::optional<std::vector<std::string>>) {}, "/photos",
      std::make_shared<std::vector<std::exception_ptr>>());
  REQUIRE(state->filters.size() == 1);
  CHECK(std::string{state->filters[0].name} == "Images");
  CHECK(std::string{state->filters[0].pattern} == "jpg;png");
  CHECK(state->default_location == "/photos");
}
