#include <catch2/catch_test_macros.hpp>

#include "basalt/paint.h"
#include "basalt/text/font_manager.h"

#include "magma/render/blend2d_painter.h"
#include "magma/render/glyph_atlas.h"
#include "magma/render/gpu_scene.h"

#include <blend2d/blend2d.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <numbers>
#include <variant>
#include <vector>

// The GPU scene is the part of the GPU backends that does not need a GPU: these tests run on
// every platform.

using namespace basalt;
using magma::pixel_rect;
using magma::render::begin_layer_pass;
using magma::render::draw_batch;
using magma::render::end_layer_pass;
using magma::render::glyph_atlas;
using magma::render::gpu_scene;
using magma::render::gpu_scene_builder;
using magma::render::instance_kind;
using magma::render::texture_ref;

namespace {

constexpr auto white = rgba{255, 255, 255, 255};
constexpr auto red = rgba{255, 0, 0, 255};

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

struct built {
  gpu_scene_builder builder;
  glyph_atlas atlas;
  gpu_scene const *scene = nullptr;
};

auto build(paint_commands const &commands, pixel_rect area = {0, 0, 100, 100}, float scale = 1)
    -> std::unique_ptr<built> {
  auto result = std::make_unique<built>();
  auto const areas = std::array{area};
  result->scene = &result->builder.build(commands, areas, scale, white, result->atlas);
  return result;
}

auto kind(magma::render::gpu_instance const &instance) -> instance_kind {
  return static_cast<instance_kind>(static_cast<int>(instance.params[2]));
}

auto batch(magma::render::gpu_op const &op) -> draw_batch {
  auto const *const found = std::get_if<draw_batch>(&op);
  REQUIRE(found != nullptr);
  return *found;
}

} // namespace

TEST_CASE("each damaged area starts with an instance that clears it", "[gpu]") {
  auto builder = gpu_scene_builder{};
  auto atlas = glyph_atlas{};
  auto const areas = std::array{pixel_rect{0, 0, 10, 10}, pixel_rect{20, 5, 4, 4}};
  auto const &scene = builder.build({}, areas, 1, {255, 0, 0, 128}, atlas);
  REQUIRE(scene.clear_count == 2);
  REQUIRE(scene.instances.size() == 2);
  CHECK(scene.ops.empty());
  CHECK(scene.instances[1].outer == std::array{20.0F, 5.0F, 24.0F, 9.0F});
  // Premultiplied: half of red.
  CHECK(scene.instances[1].color[0] == 128.0F / 255);
  CHECK(scene.instances[1].color[3] == 128.0F / 255);
}

TEST_CASE("a filled rect becomes one shape instance in device pixels", "[gpu]") {
  auto const result =
      build({fill_rect_command{{{2, 3}, {4, 5}}, {0, 0, 255, 255}}}, {0, 0, 40, 40}, 2);
  auto const &scene = *result->scene;
  REQUIRE(scene.instances.size() == 2);
  auto const &rect = scene.instances[1];
  CHECK(kind(rect) == instance_kind::shape);
  // Local edges stay in layout pixels, and the map back from device pixels undoes the scale.
  CHECK(rect.outer == std::array{2.0F, 3.0F, 6.0F, 8.0F});
  CHECK(rect.to_local_x == std::array{0.5F, 0.0F, 0.0F, 0.0F});
  CHECK(rect.to_local_y == std::array{0.0F, 0.5F, 0.0F, 0.0F});
  // Device pixels 4 to 12 and 6 to 16, with one pixel around them for the antialiased edge.
  CHECK(rect.bounds == std::array{3.0F, 5.0F, 13.0F, 17.0F});
  CHECK(rect.clip == std::array{0.0F, 0.0F, 40.0F, 40.0F});
  REQUIRE(scene.ops.size() == 1);
  CHECK(batch(scene.ops[0]).first == 1);
  CHECK(batch(scene.ops[0]).count == 1);
}

TEST_CASE("commands outside the damaged areas are left out", "[gpu]") {
  auto const result =
      build({fill_rect_command{{{50, 50}, {10, 10}}, red}, line_command{{50, 0}, {60, 0}, red, 1}},
            {0, 0, 20, 20});
  CHECK(result->scene->instances.size() == 1);
  CHECK(result->scene->ops.empty());
}

TEST_CASE("a clip limits the quad and travels with each instance", "[gpu]") {
  auto const result = build({
      push_clip_command{{{10, 10}, {5.5F, 20}}},
      fill_rect_command{{{0, 0}, {100, 100}}, red},
      pop_clip_command{},
      fill_rect_command{{{0, 0}, {100, 100}}, red},
  });
  auto const &scene = *result->scene;
  REQUIRE(scene.instances.size() == 3);
  // The quad covers every pixel the fractional clip touches; the shader cuts the last one.
  CHECK(scene.instances[1].bounds == std::array{10.0F, 10.0F, 16.0F, 30.0F});
  CHECK(scene.instances[1].clip == std::array{10.0F, 10.0F, 15.5F, 30.0F});
  CHECK(scene.instances[2].clip == std::array{0.0F, 0.0F, 100.0F, 100.0F});
  // A clip needs no new batch.
  REQUIRE(scene.ops.size() == 1);
  CHECK(batch(scene.ops[0]).count == 2);
}

TEST_CASE("a change of texture starts a new batch", "[gpu]") {
  auto const image = raster_image::create(1, 1, {255, 255, 255, 255});
  auto const result = build({
      fill_rect_command{{{0, 0}, {10, 10}}, red},
      fill_rounded_rect_command{{{10, 0}, {10, 10}}, red, 3},
      draw_image_command{{{20, 0}, {10, 10}}, image},
      draw_image_command{{{30, 0}, {10, 10}}, image},
      fill_rect_command{{{40, 0}, {10, 10}}, red},
  });
  auto const &scene = *result->scene;
  REQUIRE(scene.ops.size() == 3);
  CHECK(batch(scene.ops[0]).count == 2);
  CHECK(batch(scene.ops[1]).texture == texture_ref{texture_ref::source::image, 0});
  CHECK(batch(scene.ops[1]).count == 2);
  CHECK(batch(scene.ops[2]).texture == texture_ref{});
  REQUIRE(scene.images.size() == 1);
  CHECK(scene.images[0] == image.get());
}

TEST_CASE("strokes have a hole, and a hairline is one device pixel wide", "[gpu]") {
  auto const result = build(
      {
          stroke_rect_command{{{10, 10}, {20, 20}}, red, 2},
          stroke_rounded_rect_command{{{40, 10}, {20, 20}}, red, 4, 2},
          line_command{{0, 50}, {10, 50}, red, 0},
      },
      {0, 0, 200, 200}, 2);
  auto const &scene = *result->scene;
  REQUIRE(scene.instances.size() == 4);
  auto const &rect = scene.instances[1];
  CHECK(rect.outer == std::array{9.0F, 9.0F, 31.0F, 31.0F});
  CHECK(rect.inner == std::array{11.0F, 11.0F, 29.0F, 29.0F});
  CHECK(rect.params[3] == 1);
  auto const &rounded = scene.instances[2];
  // The outer corner grows by half the width and the inner one shrinks by it.
  CHECK(rounded.params[0] == 5);
  CHECK(rounded.params[1] == 3);
  // A line is a rect along it: 10 long and half a layout pixel (one device pixel) wide.
  auto const &line = scene.instances[3];
  CHECK(line.outer == std::array{0.0F, -0.25F, 10.0F, 0.25F});
}

TEST_CASE("a layer with an opacity is drawn in its own pass and composited", "[gpu]") {
  auto const result = build({
      push_clip_command{{{10, 10}, {20, 20}}},
      begin_layer_command{128},
      fill_rect_command{{{0, 0}, {100, 100}}, red},
      end_layer_command{},
      pop_clip_command{},
  });
  auto const &scene = *result->scene;
  REQUIRE(scene.layers.size() == 1);
  CHECK(scene.layers[0].area.x == 10);
  CHECK(scene.layers[0].area.width == 20);
  REQUIRE(scene.ops.size() == 4);
  CHECK(std::holds_alternative<begin_layer_pass>(scene.ops[0]));
  CHECK(batch(scene.ops[1]).texture == texture_ref{});
  CHECK(std::holds_alternative<end_layer_pass>(scene.ops[2]));
  auto const composite = batch(scene.ops[3]);
  CHECK(composite.texture == texture_ref{texture_ref::source::layer, 0});
  auto const &instance = scene.instances[composite.first];
  CHECK(kind(instance) == instance_kind::layer);
  CHECK(instance.color[3] == 128.0F / 255);
  CHECK(instance.inner == std::array{10.0F, 10.0F, 1.0F / 20, 1.0F / 20});
}

TEST_CASE("opaque and invisible layers need no pass, and open ones are closed", "[gpu]") {
  auto const plain = build({
      begin_layer_command{255},
      fill_rect_command{{{0, 0}, {10, 10}}, red},
      end_layer_command{},
      begin_layer_command{0},
      fill_rect_command{{{0, 0}, {10, 10}}, red},
      end_layer_command{},
  });
  CHECK(plain->scene->layers.empty());
  CHECK(plain->scene->instances.size() == 2);

  auto const unclosed =
      build({begin_layer_command{100}, fill_rect_command{{{0, 0}, {10, 10}}, red}});
  REQUIRE(unclosed->scene->ops.size() == 4);
  CHECK(std::holds_alternative<end_layer_pass>(unclosed->scene->ops[2]));
}

TEST_CASE("a rotated clip draws through a layer masked by the clip", "[gpu]") {
  auto const result = build({
      push_transform_command{affine_transform::translation(50, 50) *
                             affine_transform::rotation(std::numbers::pi_v<float> / 4)},
      push_clip_command{{{-10, -10}, {20, 20}}},
      fill_rect_command{{{-40, -40}, {80, 80}}, red},
      pop_clip_command{},
      pop_transform_command{},
  });
  auto const &scene = *result->scene;
  REQUIRE(scene.layers.size() == 1);
  REQUIRE(scene.ops.size() == 4);
  auto const &mask = scene.instances[batch(scene.ops[3]).first];
  CHECK(mask.outer == std::array{-10.0F, -10.0F, 10.0F, 10.0F});
  CHECK(mask.color[3] == 1);
}

TEST_CASE("glyphs without rotation are placed on whole device pixels", "[gpu]") {
  auto const face = fonts().match({"Noto Sans"});
  REQUIRE(face != nullptr);
  auto const letter = face->glyph_for(U'H');
  auto const space = face->glyph_for(U' ');
  REQUIRE(letter.has_value());
  REQUIRE(space.has_value());
  auto const run = std::make_shared<text::glyph_run const>(text::glyph_run{
      .face = face,
      .size = 20,
      .glyphs = {{.id = *letter, .position = {0, 0}},
                 {.id = *space, .position = {14, 0}},
                 {.id = *letter, .position = {21, 0}}},
      .advance = 35,
  });
  auto const result = build({draw_glyph_run_command{{10.25F, 30}, run, red}});
  auto const &scene = *result->scene;
  // The space has no outline, so it has no instance.
  REQUIRE(scene.instances.size() == 3);
  auto const &first = scene.instances[1];
  auto const &second = scene.instances[2];
  CHECK(kind(first) == instance_kind::glyph);
  CHECK(first.to_local_x == std::array{1.0F, 0.0F, 0.0F, 0.0F});
  // The same glyph a whole number of pixels further on uses the same bitmap.
  CHECK(first.inner == second.inner);
  CHECK(second.bounds[0] - first.bounds[0] == 21);
  CHECK(first.bounds[0] == std::floor(first.bounds[0]));
  CHECK(batch(scene.ops[0]).texture == texture_ref{texture_ref::source::atlas_page, 0});
}

TEST_CASE("the atlas rasterizes a glyph once for each size and subpixel offset", "[gpu]") {
  auto const face = fonts().match({"Noto Sans"});
  REQUIRE(face != nullptr);
  auto const letter = face->glyph_for(U'g');
  auto const space = face->glyph_for(U' ');
  REQUIRE(letter.has_value());
  REQUIRE(space.has_value());
  auto atlas = glyph_atlas{};

  auto const *const first = atlas.find(*face, *letter, 16, 0, 0);
  REQUIRE(first != nullptr);
  CHECK(atlas.find(*face, *letter, 16, 0, 0) == first);
  auto const *const shifted = atlas.find(*face, *letter, 16, 2, 0);
  REQUIRE(shifted != nullptr);
  CHECK(shifted != first);
  CHECK(atlas.find(*face, *letter, 17, 0, 0) != first);
  CHECK(atlas.find(*face, *space, 16, 0, 0) == nullptr);

  // "g" reaches below the baseline, and the bitmap has some coverage in it.
  CHECK(first->top < 0);
  CHECK(first->top + first->height > 0);
  auto const &page = atlas.pages()[first->page];
  auto covered = 0;
  for (auto y = first->y; y < first->y + first->height; ++y) {
    for (auto x = first->x; x < first->x + first->width; ++x) {
      covered += page.pixels[static_cast<std::size_t>(y * atlas.page_size() + x)] > 128 ? 1 : 0;
    }
  }
  CHECK(covered > 10);
  CHECK(page.dirty.width > 0);
  atlas.clear_dirty();
  CHECK(atlas.pages()[0].dirty.width == 0);
}

TEST_CASE("an atlas bitmap of any width has the coverage of its outline", "[gpu]") {
  auto const face = fonts().match({"Noto Sans", 700});
  REQUIRE(face != nullptr);
  auto const letter = face->glyph_for(U'R');
  REQUIRE(letter.has_value());
  auto atlas = glyph_atlas{};
  // Blend2D draws A8 rows that are not a multiple of 4 bytes wrongly, so the width must not be one.
  auto pixels_per_em = 100.0F;
  auto const *placed = atlas.find(*face, *letter, pixels_per_em, 0, 0);
  while (placed != nullptr && placed->width % 4 == 0 && pixels_per_em < 104) {
    pixels_per_em += 1;
    placed = atlas.find(*face, *letter, pixels_per_em, 0, 0);
  }
  REQUIRE(placed != nullptr);
  REQUIRE(placed->width % 4 != 0);

  auto expected = BLImage{};
  REQUIRE(expected.create(placed->width, placed->height, BL_FORMAT_PRGB32) == BL_SUCCESS);
  auto context = BLContext{expected};
  context.clear_all();
  auto const scale = static_cast<double>(pixels_per_em) / face->units_per_em();
  context.set_transform(BLMatrix2D(scale, 0, 0, scale, -placed->left, -placed->top));
  context.fill_path(magma::render::glyph_path(*face, *letter), BLRgba32(0xffffffffU));
  context.end();
  auto data = BLImageData{};
  expected.get_data(&data);

  auto const &page = atlas.pages()[placed->page];
  auto different = 0;
  for (auto y = 0; y < placed->height; ++y) {
    auto const *const row = static_cast<std::uint8_t const *>(data.pixel_data) + y * data.stride;
    for (auto x = 0; x < placed->width; ++x) {
      auto const alpha = static_cast<int>(row[x * 4 + 3]);
      auto const actual = static_cast<int>(page.pixels[static_cast<std::size_t>(
          (placed->y + y) * atlas.page_size() + placed->x + x)]);
      different += std::abs(actual - alpha) > 2 ? 1 : 0;
    }
  }
  CHECK(different == 0);
}

TEST_CASE("the atlas starts over when it has more pages than its limit", "[gpu]") {
  auto const face = fonts().match({"Noto Sans"});
  REQUIRE(face != nullptr);
  auto atlas = glyph_atlas{64, 1};
  for (auto letter = U'A'; letter <= U'Z' && atlas.pages().size() < 2; ++letter) {
    auto const glyph = face->glyph_for(letter);
    REQUIRE(glyph.has_value());
    CHECK(atlas.find(*face, *glyph, 40, 0, 0) != nullptr);
  }
  REQUIRE(atlas.pages().size() == 2);
  auto const first_id = atlas.pages()[0].id;
  atlas.begin_frame();
  CHECK(atlas.pages().empty());
  auto const glyph = face->glyph_for(U'A');
  REQUIRE(glyph.has_value());
  CHECK(atlas.find(*face, *glyph, 40, 0, 0) != nullptr);
  CHECK(atlas.pages()[0].id != first_id);

  // A bitmap larger than a page cannot be placed.
  CHECK(atlas.find(*face, *glyph, 200, 0, 0) == nullptr);
}
