#pragma once

#include "basalt/color.h"
#include "basalt/geometry.h"
#include "basalt/image.h"
#include "basalt/paint.h"

#include "magma/platform.h"

#include <array>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <variant>
#include <vector>

#include "glyph_atlas.h"

namespace magma::render {

// What the shader draws for an instance. The values are shared with the shaders.
enum class instance_kind : std::uint8_t {
  shape = 0, // A rect, rounded rect, stroke or line.
  glyph = 1, // Coverage from an atlas page, in the instance color.
  image = 2, // A premultiplied image, scaled to the outer rect.
  layer = 3, // A finished layer, through the outer rect as a mask.
};

// One quad. Every backend uses this layout in its shaders: eight rows of four floats.
//
// The quad covers `bounds` in frame device pixels. The shader maps each pixel center to local
// coordinates with `to_local_x` and `to_local_y` (u = x * [0] + y * [1] + [2]), and measures
// coverage there, so a rotated quad keeps exact edges.
struct gpu_instance {
  std::array<float, 4> bounds{};     // Device: left, top, right, bottom.
  std::array<float, 4> outer{};      // Local: left, top, right, bottom.
  std::array<float, 4> inner{};      // Shape: the hole of a stroke. Glyph and image: the texture
                                     // coordinates of the outer rect. Layer: the layer's device
                                     // left and top, and 1 / width and 1 / height.
  std::array<float, 4> params{};     // Outer radius, inner radius, kind, 1 when there is a hole.
  std::array<float, 4> color{};      // Premultiplied, 0 to 1. A layer's opacity in each channel.
  std::array<float, 4> to_local_x{}; //
  std::array<float, 4> to_local_y{}; //
  std::array<float, 4> clip{};       // Device: left, top, right, bottom.
};
static_assert(sizeof(gpu_instance) == 128);

// The texture a batch samples.
struct texture_ref {
  enum class source : std::uint8_t { none, atlas_page, image, layer };
  source from = source::none;
  // An index into the atlas pages, the scene images or the scene layers.
  std::uint32_t index = 0;

  [[nodiscard]] friend auto operator==(texture_ref const &, texture_ref const &) -> bool = default;
};

// Instances [first, first + count) drawn with one texture. Clips need no scissor: each instance
// has its clip, and its quad stays inside it. `bounds` covers the quads of all of them, in frame
// device pixels.
struct draw_batch {
  texture_ref texture;
  std::uint32_t first = 0;
  std::uint32_t count = 0;
  pixel_rect bounds;
};

// The draws that follow go into layer `layer`, cleared to transparent, until the matching end.
// `end` is the index of the operation after the matching `end_layer_pass`.
struct begin_layer_pass {
  std::uint32_t layer = 0;
  std::uint32_t end = 0;
};

// The draws that follow go back to the target that was active before the matching begin.
struct end_layer_pass {};

using gpu_op = std::variant<draw_batch, begin_layer_pass, end_layer_pass>;

// An offscreen texture that a group with an opacity, or a clip that is not axis-aligned, draws
// into. `area` is in frame device pixels.
struct gpu_layer {
  pixel_rect area;
};

// One frame for a GPU backend.
struct gpu_scene {
  // The first `clear_count` instances clear one damaged area each, in order. Draw them without
  // blending.
  std::vector<gpu_instance> instances;
  std::size_t clear_count = 0;
  std::vector<gpu_op> ops;
  std::vector<gpu_layer> layers;
  // The images that batches sample, valid while the paint commands are.
  std::vector<basalt::raster_image const *> images;
};

// Turns Basalt paint commands into instances and batches. It follows the same rules as the
// Blend2D painter, which is the reference: the same culling, clipping and layer bounds.
class gpu_scene_builder {
public:
  // Builds the scene for `areas`, which are in frame device pixels. Commands are in layout
  // pixels and are scaled by `scale`. Glyphs come from `atlas`, which must not be reset until
  // the frame has been drawn.
  [[nodiscard]] auto build(basalt::paint_commands const &commands,
                           std::span<pixel_rect const> areas, float scale, basalt::rgba clear,
                           glyph_atlas &atlas) -> gpu_scene const &;

private:
  gpu_scene scene_;
  std::unordered_map<std::uint64_t, std::uint32_t> image_indexes_;
};

} // namespace magma::render
