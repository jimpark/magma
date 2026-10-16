#include <catch2/catch_test_macros.hpp>

#include "basalt/event.h"

#include "magma/platform.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
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

TEST_CASE("wheel notches become layout pixels in the wheel's direction", "[platform][events]") {
  auto platform = make_platform();
  auto const window = make_window(platform, {100, 100});
  auto event = SDL_Event{};
  event.wheel.type = SDL_EVENT_MOUSE_WHEEL;
  event.wheel.windowID = window;
  event.wheel.y = 1;
  event.wheel.mouse_x = 10;
  event.wheel.mouse_y = 20;
  event.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
  push(event);
  event.wheel.direction = SDL_MOUSEWHEEL_FLIPPED;
  push(event);

  auto wheels = std::vector<basalt::wheel_event>{};
  for (auto const &entry : platform.poll_events()) {
    if (auto const *const wheel = std::get_if<basalt::wheel_event>(&entry.event)) {
      wheels.push_back(*wheel);
    }
  }
  REQUIRE(wheels.size() == 2);
  CHECK(wheels[0].position == basalt::point{10, 20});
  CHECK(wheels[0].delta == basalt::point{0, 40});
  CHECK(wheels[0].precise);
  CHECK(wheels[1].delta == basalt::point{0, -40});
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
