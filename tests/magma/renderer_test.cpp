#include <catch2/catch_test_macros.hpp>

#include "basalt/paint.h"

#include "magma/platform.h"
#include "magma/renderer.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <utility>
#include <vector>

using namespace basalt;

namespace {

constexpr auto white = rgba{255, 255, 255, 255};
constexpr auto red = rgba{255, 0, 0, 255};
constexpr auto blue = rgba{0, 0, 255, 255};

auto fill(rgba color) -> paint_commands {
  return {fill_rect_command{{{0, 0}, {1000, 1000}}, color}};
}

auto offscreen(size viewport, float scale = 1) -> magma::renderer {
  auto renderer = magma::renderer::create_offscreen();
  renderer.set_metrics(viewport, scale);
  return renderer;
}

auto make_platform() -> magma::platform {
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
  auto created = magma::platform::create();
  REQUIRE(created.has_value());
  return std::move(*created);
}

} // namespace

TEST_CASE("an offscreen renderer draws on the CPU and has no frame until it renders", "[render]") {
  auto renderer = offscreen({20, 10});
  CHECK(renderer.active_backend() == magma::backend::raster);
  CHECK(renderer.read_frame() == nullptr);
  CHECK_FALSE(renderer.read_pixel(0, 0).has_value());
  CHECK(renderer.last_frame() == 0);
  CHECK(renderer.needs_full_redraw());
}

TEST_CASE("the frame is the viewport times the scale, in whole device pixels", "[render]") {
  auto renderer = offscreen({20.4F, 10}, 1.5F);
  CHECK(renderer.viewport() == size{20.4F, 10});
  CHECK(renderer.scale() == 1.5F);
  CHECK(renderer.pixel_size() == size{31, 15});
  REQUIRE(renderer.render(fill(red), {}, white).has_value());
  auto const frame = renderer.read_frame();
  REQUIRE(frame != nullptr);
  CHECK(frame->width() == 31);
  CHECK(frame->height() == 15);
  CHECK(renderer.read_pixel(30, 14) == red);
  CHECK_FALSE(renderer.read_pixel(31, 0).has_value());
}

TEST_CASE("a full redraw ignores the damage and clears to the clear color", "[render]") {
  auto renderer = offscreen({20, 10});
  auto const damage = std::array{rect{{0, 0}, {1, 1}}};
  auto const commands = paint_commands{fill_rect_command{{{10, 0}, {10, 10}}, blue}};
  REQUIRE(renderer.render(commands, damage, red).has_value());
  CHECK(renderer.read_pixel(5, 5) == red);
  CHECK(renderer.read_pixel(15, 5) == blue);
  CHECK_FALSE(renderer.needs_full_redraw());
  CHECK(renderer.last_frame() == 1);
}

TEST_CASE("without damage or a pending full redraw nothing is drawn", "[render]") {
  auto renderer = offscreen({20, 10});
  REQUIRE(renderer.render(fill(red), {}, white).has_value());
  REQUIRE(renderer.render(fill(blue), {}, white).has_value());
  CHECK(renderer.read_pixel(5, 5) == red);
  CHECK(renderer.last_frame() == 1);
}

TEST_CASE("a partial redraw changes only the damaged area", "[render]") {
  auto renderer = offscreen({20, 10});
  REQUIRE(renderer.render(fill(red), {}, white).has_value());
  auto const damage = std::array{rect{{2, 2}, {3, 3}}, rect{{12, 6}, {2, 2}}};
  REQUIRE(renderer.render(fill(blue), damage, white).has_value());
  CHECK(renderer.read_pixel(2, 2) == blue);
  CHECK(renderer.read_pixel(4, 4) == blue);
  CHECK(renderer.read_pixel(13, 7) == blue);
  CHECK(renderer.read_pixel(5, 5) == red);
  CHECK(renderer.read_pixel(1, 1) == red);
  CHECK(renderer.read_pixel(14, 8) == red);
  CHECK(renderer.last_frame() == 2);
}

TEST_CASE("damage is rounded out to whole device pixels", "[render]") {
  auto renderer = offscreen({20, 10}, 2);
  REQUIRE(renderer.render(fill(red), {}, white).has_value());
  auto const damage = std::array{rect{{1.3F, 1.3F}, {1, 1}}};
  REQUIRE(renderer.render({}, damage, white).has_value());
  // 1.3 to 2.3 layout pixels is 2.6 to 4.6 device pixels, so pixels 2 to 4 are cleared.
  CHECK(renderer.read_pixel(1, 1) == red);
  CHECK(renderer.read_pixel(2, 2) == white);
  CHECK(renderer.read_pixel(4, 4) == white);
  CHECK(renderer.read_pixel(5, 5) == red);
}

TEST_CASE("changing the device size or scale requests a full redraw", "[render]") {
  auto renderer = offscreen({20, 10});
  REQUIRE(renderer.render(fill(red), {}, white).has_value());
  renderer.set_metrics({20, 10}, 1);
  CHECK_FALSE(renderer.needs_full_redraw());
  renderer.set_metrics({30, 10}, 1);
  CHECK(renderer.needs_full_redraw());
  REQUIRE(renderer.render(fill(red), {}, white).has_value());
  renderer.set_metrics({15, 5}, 2);
  CHECK(renderer.needs_full_redraw());
  REQUIRE(renderer.render(fill(red), {}, white).has_value());
  renderer.request_full_redraw();
  CHECK(renderer.needs_full_redraw());
}

TEST_CASE("an empty viewport renders nothing", "[render]") {
  auto renderer = offscreen({0, 10});
  REQUIRE(renderer.render(fill(red), {}, white).has_value());
  CHECK(renderer.last_frame() == 0);
}

TEST_CASE("a window renderer presents its frame in the window", "[render][platform]") {
  auto platform = make_platform();
  auto config = magma::window_config{};
  config.graphics = magma::preferred_graphics_api(magma::backend_preference::automatic);
  config.size = {16, 8};
  auto const window = platform.create_window(config);
  REQUIRE(window.has_value());

  auto renderer = magma::renderer::create(platform, *window);
  REQUIRE(renderer.has_value());
  CHECK(renderer->pixel_size() == size{16, 8});
  REQUIRE(renderer->render(fill(red), {}, white).has_value());
  auto const damage = std::array{rect{{0, 0}, {2, 2}}};
  REQUIRE(renderer->render(fill(blue), damage, white).has_value());

  auto *const surface = SDL_GetWindowSurface(SDL_GetWindowFromID(*window));
  REQUIRE(surface != nullptr);
  auto const color_at = [surface](int x, int y) {
    auto color = rgba{};
    REQUIRE(SDL_ReadSurfacePixel(surface, x, y, &color.r, &color.g, &color.b, &color.a));
    return color;
  };
  CHECK(color_at(1, 1) == blue);
  CHECK(color_at(10, 5) == red);
}

TEST_CASE("a renderer for an unknown window is refused", "[render][platform]") {
  auto platform = make_platform();
  CHECK_FALSE(magma::renderer::create(platform, 9999).has_value());
}

TEST_CASE("presentations are recorded only while tracking is on", "[render]") {
  auto renderer = offscreen({4, 4});
  REQUIRE(renderer.render(fill(red), {}, white).has_value());
  CHECK(renderer.take_presentations().empty());

  renderer.track_presentations(true);
  auto const before = magma::platform::event_clock_now();
  renderer.request_full_redraw();
  REQUIRE(renderer.render(fill(red), {}, white).has_value());
  renderer.request_full_redraw();
  REQUIRE(renderer.render(fill(red), {}, white).has_value());
  auto const presented = renderer.take_presentations();
  REQUIRE(presented.size() == 2);
  CHECK(presented[0].frame == 2);
  CHECK(presented[1].frame == 3);
  REQUIRE(presented[0].time.has_value());
  CHECK(*presented[0].time >= before);
  CHECK(renderer.take_presentations().empty());

  renderer.track_presentations(false);
  renderer.request_full_redraw();
  REQUIRE(renderer.render(fill(red), {}, white).has_value());
  CHECK(renderer.take_presentations().empty());
}

TEST_CASE("a large frame drawn at once matches the same frame drawn in strips", "[render]") {
  // Large areas are drawn on several threads and small ones on one. Clipping to a strip can
  // change the rounding of a pixel, but by no more than 2 levels.
  auto commands = paint_commands{};
  for (auto i = 0; i < 40; ++i) {
    auto const offset = static_cast<float>(i) * 23.7F;
    commands.emplace_back(fill_rounded_rect_command{
        {{offset, offset * 0.6F}, {180, 90}}, {static_cast<std::uint8_t>(i * 6), 90, 160, 200}, 9});
    commands.emplace_back(line_command{{0, offset}, {1000, 700 - offset}, {20, 20, 20, 255}, 1.5F});
  }
  auto whole = offscreen({1000, 700});
  REQUIRE(whole.render(commands, {}, white).has_value());

  auto strips = offscreen({1000, 700});
  REQUIRE(strips.render({}, {}, white).has_value());
  auto damage = std::vector<rect>{};
  for (auto y = 0; y < 700; y += 50) {
    damage.push_back({{0, static_cast<float>(y)}, {1000, 50}});
  }
  for (auto const &strip : damage) {
    REQUIRE(strips.render(commands, std::span{&strip, 1}, white).has_value());
  }
  auto const expected = strips.read_frame();
  auto const actual = whole.read_frame();
  REQUIRE(expected != nullptr);
  REQUIRE(actual != nullptr);
  REQUIRE(actual->pixels().size() == expected->pixels().size());
  auto largest = 0;
  for (auto i = std::size_t{0}; i < actual->pixels().size(); ++i) {
    largest = std::max(largest, std::abs(actual->pixels()[i] - expected->pixels()[i]));
  }
  CHECK(largest <= 2);
}
