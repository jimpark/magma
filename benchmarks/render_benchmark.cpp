// Benchmarks for replaying paint commands. The scene is a page of an article: a heading, body
// text, a code block and a few rules, the size of a laptop window. The cases match what the
// application does while scrolling (redraw everything), typing (redraw one line) and blinking
// the caret (redraw a sliver).
//
// Each case runs on the CPU and, when the computer has one, on the GPU backend. A GPU frame is
// timed until the GPU has finished it, by reading back one pixel.
//
// Run them with `just bench`. Catch2 warms up, estimates the clock cost, takes many samples and
// reports a bootstrap confidence interval. Compare two runs by their means and intervals; a change
// is real only when the intervals do not overlap.
#include <catch2/benchmark/catch_benchmark.hpp>
#include <catch2/catch_test_macros.hpp>

#include "basalt/paint.h"
#include "basalt/text/font_manager.h"
#include "basalt/text/text_shaper.h"
#include "basalt/typography.h"

#include "magma/renderer.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace basalt;

namespace {

constexpr auto viewport = size{1200, 800};
constexpr auto line_height = 24.0F;
constexpr auto white = rgba{255, 255, 255, 255};

// Debug builds and sanitizers measure the instrumentation, so they are refused.
auto require_optimized_build() -> void {
#ifndef NDEBUG
  FAIL("Benchmarks need an optimized build. Use `just bench`.");
#endif
}

auto fonts() -> text::font_manager & {
  static auto const manager = [] {
    auto created = std::make_unique<text::font_manager>();
    created->add_file(std::filesystem::path{MAGMA_TEST_FONT_DIR} /
                      "NotoSans-VariableFont_wdth,wght.ttf");
    return created;
  }();
  return *manager;
}

auto add_text(paint_commands &commands, std::string_view utf8, point origin, float size, rgba color,
              font_weight weight = font_weight::regular) -> void {
  auto options = text::shape_options{};
  options.style = text_style{.family = "Noto Sans", .font_size = size, .weight = weight};
  for (auto const &placed : text::text_shaper{fonts()}.shape(utf8, options).runs) {
    commands.emplace_back(
        draw_glyph_run_command{{origin.x + placed.x, origin.y}, placed.run, color});
  }
}

// About 30 lines of body text, as one page of an article scrolled by `offset`.
auto article(float offset) -> paint_commands {
  constexpr auto body = std::array<std::string_view, 6>{
      "The editor keeps every paragraph shaped and laid out, so a frame only replays commands.",
      "Scrolling moves the whole page, which makes each frame a full redraw of the viewport.",
      "Typing changes one line at a time, and the renderer redraws only the damaged area.",
      "Text is most of what an article draws: glyph runs, one for each line and each style.",
      "Rules, quotes and code blocks add rects and rounded rects between the paragraphs.",
      "The caret blinks twice a second and damages a rect a few pixels wide on each blink.",
  };
  auto commands = paint_commands{};
  commands.emplace_back(push_clip_command{{{0, 0}, viewport}});
  commands.emplace_back(push_transform_command{affine_transform::translation(0, -offset)});
  add_text(commands, "Rendering an article with Magma", {64, 64}, 32, {20, 20, 20, 255},
           font_weight::bold);
  commands.emplace_back(line_command{{64, 84}, {1136, 84}, {200, 200, 200, 255}, 1});
  auto y = 120.0F;
  for (auto line = std::size_t{0}; line < 30; ++line) {
    if (line == 12) {
      commands.emplace_back(fill_rounded_rect_command{
          {{56, y - 18}, {1088, line_height * 4 + 12}}, {245, 245, 245, 255}, 6});
      commands.emplace_back(stroke_rounded_rect_command{
          {{56, y - 18}, {1088, line_height * 4 + 12}}, {220, 220, 220, 255}, 6, 1});
    }
    auto const color = line >= 12 && line < 16 ? rgba{120, 40, 160, 255} : rgba{30, 30, 30, 255};
    add_text(commands, body[line % body.size()], {64, y}, 16, color);
    y += line_height;
  }
  commands.emplace_back(pop_transform_command{});
  commands.emplace_back(pop_clip_command{});
  // The caret.
  commands.emplace_back(fill_rect_command{{{420, 300}, {2, 20}}, {0, 90, 200, 255}});
  return commands;
}

auto warmed_renderer(magma::backend_preference preference, float scale,
                     paint_commands const &commands) -> magma::renderer {
  auto renderer = magma::renderer::create_offscreen(preference);
  renderer.set_metrics(viewport, scale);
  REQUIRE(renderer.render(commands, {}, white).has_value());
  return renderer;
}

auto backend_name(magma::renderer const &renderer) -> std::string_view {
  return renderer.active_backend() == magma::backend::metal ? "metal" : "raster";
}

// Renders and waits until the frame is finished.
auto render(magma::renderer &renderer, paint_commands const &commands, std::span<rect const> damage)
    -> bool {
  return renderer.render(commands, damage, white).has_value() &&
         (renderer.active_backend() == magma::backend::raster ||
          renderer.read_pixel(0, 0).has_value());
}

// The backends to measure: the CPU, and the GPU when there is one.
auto backends() -> std::vector<magma::backend_preference> {
  auto result = std::vector{magma::backend_preference::raster_only};
  if (magma::renderer::create_offscreen(magma::backend_preference::automatic).active_backend() !=
      magma::backend::raster) {
    result.push_back(magma::backend_preference::automatic);
  }
  return result;
}

} // namespace

TEST_CASE("replay an article page", "[render][benchmark]") {
  require_optimized_build();
  auto const page = article(0);
  auto const scrolled = article(37);

  for (auto const preference : backends()) {
    for (auto const scale : {1.0F, 2.0F}) {
      auto renderer = warmed_renderer(preference, scale, page);
      auto const name = [&renderer, scale](std::string_view what) {
        return std::string{what} + (scale == 1 ? " at scale 1 (" : " at scale 2 (") +
               std::string{backend_name(renderer)} + ")";
      };

      BENCHMARK(name("full redraw while scrolling")) {
        renderer.request_full_redraw();
        return render(renderer, scrolled, {});
      };

      auto const typed_line = std::array{rect{{0, 102}, {viewport.width, line_height}}};
      BENCHMARK(name("one line while typing")) { return render(renderer, page, typed_line); };

      auto const caret = std::array{rect{{419, 299}, {4, 22}}};
      BENCHMARK(name("caret blink")) { return render(renderer, page, caret); };
    }
  }
}

TEST_CASE("first frame of an article page", "[render][benchmark]") {
  require_optimized_build();
  auto const page = article(0);
  // A new renderer has empty glyph and image caches, as at startup. For the GPU this includes
  // creating the device objects and compiling the shaders.
  for (auto const preference : backends()) {
    auto const name = std::string{"first frame at scale 2 ("} +
                      (preference == magma::backend_preference::raster_only ? "raster" : "metal") +
                      ")";
    BENCHMARK(std::string{name}) {
      auto renderer = magma::renderer::create_offscreen(preference);
      renderer.set_metrics(viewport, 2);
      return render(renderer, page, {});
    };
  }
}
