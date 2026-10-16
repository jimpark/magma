#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "basalt/paint.h"
#include "basalt/text/font_manager.h"
#include "basalt/text/text_shaper.h"
#include "basalt/typography.h"

#include "magma/renderer.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <numbers>
#include <string_view>
#include <utility>
#include <vector>

#include "golden.h"

// Each test renders a fixed list of paint commands and compares the frame with a stored image.
// Together they use every paint command. Run with MAGMA_UPDATE_GOLDEN=1 to write the images.
//
// Each test runs twice: on the CPU, which made the golden images, and on the GPU backend when the
// machine has one. GPU frames are compared with the same images, within a wider tolerance.

using namespace basalt;
using magma::backend_preference;

namespace {

constexpr auto white = rgba{255, 255, 255, 255};
constexpr auto black = rgba{0, 0, 0, 255};
constexpr auto red = rgba{220, 40, 40, 255};
constexpr auto green = rgba{40, 160, 60, 255};
constexpr auto blue = rgba{40, 80, 220, 255};

auto fonts() -> text::font_manager & {
  static auto const manager = [] {
    auto created = std::make_unique<text::font_manager>();
    auto const path =
        std::filesystem::path{MAGMA_TEST_FONT_DIR} / "NotoSans-VariableFont_wdth,wght.ttf";
    REQUIRE(created->add_file(path) > 0);
    return created;
  }();
  return *manager;
}

auto words(std::string_view utf8, point origin, float size, rgba color,
           font_weight weight = font_weight::regular) -> paint_commands {
  auto options = text::shape_options{};
  options.style = text_style{.family = "Noto Sans", .font_size = size, .weight = weight};
  auto const shaped = text::text_shaper{fonts()}.shape(utf8, options);
  auto commands = paint_commands{};
  for (auto const &placed : shaped.runs) {
    commands.emplace_back(
        draw_glyph_run_command{{origin.x + placed.x, origin.y}, placed.run, color});
  }
  return commands;
}

auto append(paint_commands &commands, paint_commands const &more) -> void {
  commands.insert(commands.end(), more.begin(), more.end());
}

// Which edges a scene has, which sets how far a GPU frame may be from the CPU reference.
enum class edges {
  // Straight edges and images: the GPU computes the same coverage as the CPU.
  straight,
  // Curves and text. The CPU flattens curves into short lines inside the curve, and the GPU puts
  // glyphs on quarter pixels, so many pixels on an edge differ, by up to about an eighth of
  // their coverage. Pixels inside shapes must still match.
  curved,
};

auto gpu_tolerance(edges kind) -> magma::test::golden_tolerance {
  return kind == edges::straight
             ? magma::test::golden_tolerance{}
             : magma::test::golden_tolerance{.outlier_fraction = 0.15, .outlier_channel = 36};
}

auto backend_name(magma::backend backend) -> std::string_view {
  switch (backend) {
  case magma::backend::raster:
    return "raster";
  case magma::backend::metal:
    return "metal";
  case magma::backend::vulkan:
    return "vulkan";
  case magma::backend::d3d12:
    return "d3d12";
  }
  return "unknown";
}

// A renderer for `preference`. Skips the test when it asks for a GPU and the machine has none.
auto make_renderer(backend_preference preference) -> magma::renderer {
  auto renderer = magma::renderer::create_offscreen(preference);
  if (preference != backend_preference::raster_only &&
      renderer.active_backend() == magma::backend::raster) {
    SKIP("this machine has no GPU backend");
  }
  return renderer;
}

struct rendered {
  raster_image_ptr frame;
  magma::backend backend = magma::backend::raster;
};

auto render(backend_preference preference, size viewport, float scale,
            paint_commands const &commands) -> rendered {
  auto renderer = make_renderer(preference);
  renderer.set_metrics(viewport, scale);
  REQUIRE(renderer.render(commands, {}, white).has_value());
  auto frame = renderer.read_frame();
  REQUIRE(frame != nullptr);
  return {std::move(frame), renderer.active_backend()};
}

auto check_golden(std::string_view name, rendered const &result, edges kind = edges::straight)
    -> void {
  auto const compared =
      result.backend == magma::backend::raster
          ? magma::test::compare_with_golden(name, *result.frame)
          : magma::test::compare_with_golden(name, *result.frame, gpu_tolerance(kind),
                                             backend_name(result.backend));
  INFO(compared.message);
  CHECK(compared.matched);
}

auto rect_scene() -> paint_commands {
  return {
      fill_rect_command{{{8, 8}, {32, 20}}, blue},
      stroke_rect_command{{{48.5F, 8.5F}, {39, 19}}, red, 1},
      stroke_rect_command{{{50, 38}, {36, 16}}, green, 3},
      fill_rect_command{{{8, 36}, {24, 20}}, {220, 40, 40, 128}},
      fill_rect_command{{{20, 44}, {24, 16}}, {40, 80, 220, 128}},
      // Fractional edges are antialiased.
      fill_rect_command{{{4.5F, 30.25F}, {30, 2.5F}}, black},
  };
}

} // namespace

TEST_CASE("filled and stroked rects match their golden image", "[render][golden]") {
  auto const preference = GENERATE(backend_preference::raster_only, backend_preference::automatic);
  check_golden("rects", render(preference, {96, 64}, 1, rect_scene()));
}

TEST_CASE("rounded rects match their golden image", "[render][golden]") {
  auto const preference = GENERATE(backend_preference::raster_only, backend_preference::automatic);
  auto const commands = paint_commands{
      fill_rounded_rect_command{{{8, 8}, {36, 24}}, {120, 60, 200, 255}, 6},
      stroke_rounded_rect_command{{{52, 8}, {36, 24}}, {230, 130, 20, 255}, 6, 2},
      // A radius larger than half the height makes a pill.
      fill_rounded_rect_command{{{8, 38}, {36, 18}}, green, 50},
      stroke_rounded_rect_command{{{52.5F, 38.5F}, {35, 17}}, black, 0, 1},
  };
  check_golden("rounded_rects", render(preference, {96, 64}, 1, commands), edges::curved);
}

TEST_CASE("lines match their golden image", "[render][golden]") {
  auto const preference = GENERATE(backend_preference::raster_only, backend_preference::automatic);
  auto const commands = paint_commands{
      line_command{{8, 8.5F}, {88, 8.5F}, black, 1},
      line_command{{8, 16}, {88, 16}, blue, 2},
      line_command{{8, 24}, {88, 56}, red, 1},
      line_command{{8, 56}, {88, 24}, {40, 160, 60, 160}, 4},
      // Width zero is a hairline.
      line_command{{48.5F, 20}, {48.5F, 60}, black, 0},
  };
  check_golden("lines", render(preference, {96, 64}, 1, commands));
}

TEST_CASE("text matches its golden image, in each weight", "[render][golden]") {
  auto const preference = GENERATE(backend_preference::raster_only, backend_preference::automatic);
  auto commands = paint_commands{};
  append(commands, words("Magma", {6, 22}, 18, black));
  append(commands, words("Magma", {80, 22}, 18, black, font_weight::bold));
  append(commands, words("The quick brown fox jumps", {6, 42}, 11, blue));
  append(commands, words("over the lazy dog.", {6, 58}, 11, red, font_weight::semibold));
  check_golden("text", render(preference, {156, 64}, 1, commands), edges::curved);
}

TEST_CASE("images match their golden image when scaled", "[render][golden]") {
  auto const preference = GENERATE(backend_preference::raster_only, backend_preference::automatic);
  // Four colors on the top rows and a fading alpha on the bottom rows.
  auto pixels = std::vector<std::uint8_t>{
      255, 0,   0, 255, 0,   255, 0,   255, 0,   0,   255, 255, 0,   0,   0, 255, //
      0,   255, 0, 255, 0,   0,   255, 255, 0,   0,   0,   255, 255, 0,   0, 255, //
      0,   0,   0, 255, 0,   0,   0,   192, 0,   0,   0,   128, 0,   0,   0, 64,  //
      200, 100, 0, 255, 200, 100, 0,   192, 200, 100, 0,   128, 200, 100, 0, 0,
  };
  auto const image = raster_image::create(4, 4, std::move(pixels));
  REQUIRE(image != nullptr);
  auto const commands = paint_commands{
      draw_image_command{{{8, 8}, {32, 32}}, image},
      draw_image_command{{{48, 8}, {4, 4}}, image},
      draw_image_command{{{48, 20}, {40, 10}}, image},
      draw_image_command{{{48, 40}, {40, 10}}, nullptr},
  };
  check_golden("images", render(preference, {96, 48}, 1, commands));
}

TEST_CASE("clips nest and unbalanced commands are tolerated", "[render][golden]") {
  auto const preference = GENERATE(backend_preference::raster_only, backend_preference::automatic);
  auto const commands = paint_commands{
      push_clip_command{{{8, 8}, {48, 32}}},
      fill_rect_command{{{0, 0}, {96, 64}}, {230, 230, 230, 255}},
      push_clip_command{{{32, 20}, {48, 40}}},
      fill_rect_command{{{0, 0}, {96, 64}}, blue},
      pop_clip_command{},
      fill_rect_command{{{4, 4}, {20, 20}}, red},
      pop_clip_command{},
      // Pops without a push do nothing.
      pop_clip_command{},
      fill_rect_command{{{60, 4}, {8, 8}}, green},
      // A push that is never popped still clips what follows it.
      push_clip_command{{{70, 44}, {10, 10}}},
      fill_rect_command{{{60, 40}, {30, 20}}, black},
  };
  check_golden("clips", render(preference, {96, 64}, 1, commands));
}

TEST_CASE("transforms and clips inside a rotation match their golden image", "[render][golden]") {
  auto const preference = GENERATE(backend_preference::raster_only, backend_preference::automatic);
  constexpr auto quarter_turn = std::numbers::pi_v<float> / 6;
  auto const commands = paint_commands{
      push_transform_command{affine_transform::translation(4, 4)},
      fill_rect_command{{{0, 0}, {12, 12}}, blue},
      push_transform_command{affine_transform::scaling(2, 1)},
      fill_rect_command{{{10, 0}, {8, 12}}, green},
      pop_transform_command{},
      pop_transform_command{},
      // A rotated square around (24, 44).
      push_transform_command{affine_transform::translation(24, 44) *
                             affine_transform::rotation(quarter_turn)},
      fill_rect_command{{{-10, -10}, {20, 20}}, red},
      pop_transform_command{},
      // A rotated clip around (72, 40) cuts a large rect to a rotated square.
      push_transform_command{affine_transform::translation(72, 40) *
                             affine_transform::rotation(quarter_turn)},
      push_clip_command{{{-12, -12}, {24, 24}}},
      fill_rect_command{{{-40, -40}, {80, 80}}, black},
      pop_clip_command{},
      pop_transform_command{},
  };
  check_golden("transforms", render(preference, {96, 64}, 1, commands));
}

TEST_CASE("layers apply their opacity to the group as a whole", "[render][golden]") {
  auto const preference = GENERATE(backend_preference::raster_only, backend_preference::automatic);
  auto const overlapping = [](float x) {
    return paint_commands{
        fill_rect_command{{{x, 8}, {24, 24}}, blue},
        fill_rect_command{{{x + 12, 20}, {24, 24}}, blue},
    };
  };
  auto commands = paint_commands{};
  // A group at half opacity: the overlap is no darker than the rest.
  commands.emplace_back(begin_layer_command{128});
  append(commands, overlapping(4));
  commands.emplace_back(end_layer_command{});
  // Each rect at half opacity: the overlap is darker.
  commands.emplace_back(fill_rect_command{{{48, 8}, {24, 24}}, {40, 80, 220, 128}});
  commands.emplace_back(fill_rect_command{{{60, 20}, {24, 24}}, {40, 80, 220, 128}});
  // Nested groups multiply, an invisible group draws nothing, and an opaque one draws as usual.
  commands.emplace_back(begin_layer_command{128});
  commands.emplace_back(begin_layer_command{128});
  commands.emplace_back(fill_rect_command{{{4, 50}, {20, 10}}, red});
  commands.emplace_back(end_layer_command{});
  commands.emplace_back(end_layer_command{});
  commands.emplace_back(begin_layer_command{0});
  commands.emplace_back(fill_rect_command{{{28, 50}, {20, 10}}, red});
  commands.emplace_back(end_layer_command{});
  commands.emplace_back(begin_layer_command{255});
  commands.emplace_back(fill_rect_command{{{52, 50}, {20, 10}}, red});
  commands.emplace_back(end_layer_command{});
  // A layer inside a clip is clipped too.
  commands.emplace_back(push_clip_command{{{76, 50}, {8, 10}}});
  commands.emplace_back(begin_layer_command{128});
  commands.emplace_back(fill_rect_command{{{76, 50}, {16, 10}}, red});
  commands.emplace_back(end_layer_command{});
  commands.emplace_back(pop_clip_command{});
  check_golden("layers", render(preference, {96, 64}, 1, commands));
}

TEST_CASE("a frame at scale 2 matches its golden image", "[render][golden]") {
  auto const preference = GENERATE(backend_preference::raster_only, backend_preference::automatic);
  auto commands = rect_scene();
  append(commands, words("Ag", {60, 24}, 14, black));
  check_golden("rects_scale_2", render(preference, {96, 64}, 2, commands), edges::curved);
}

TEST_CASE("glyphs that overlap in one run are both drawn", "[render]") {
  auto const preference = GENERATE(backend_preference::raster_only, backend_preference::automatic);
  // A run is filled as one path, so where two glyphs overlap they must not cancel out.
  auto const face = fonts().match({"Noto Sans"});
  REQUIRE(face != nullptr);
  auto const stem = face->glyph_for(U'l');
  REQUIRE(stem.has_value());
  // At 80 pixels the stem of "l" is about 7 pixels wide. The copies overlap by about 5.
  auto const run = std::make_shared<text::glyph_run const>(text::glyph_run{
      .face = face,
      .size = 80,
      .glyphs = {{.id = *stem, .position = {0, 0}}, {.id = *stem, .position = {2, 0}}},
  });
  auto renderer = make_renderer(preference);
  renderer.set_metrics({40, 90}, 1);
  REQUIRE(renderer.render({draw_glyph_run_command{{10, 85}, run, black}}, {}, white).has_value());
  auto dark = 0;
  for (auto x = 0; x < 40; ++x) {
    dark += renderer.read_pixel(x, 50).value_or(white).r < 64 ? 1 : 0;
  }
  CHECK(dark >= 7);
}
