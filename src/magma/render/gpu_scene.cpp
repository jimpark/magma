#include "gpu_scene.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <type_traits>
#include <utility>

namespace magma::render {
namespace {

using basalt::affine_transform;
using basalt::rect;

auto is_empty(pixel_rect area) -> bool { return area.width <= 0 || area.height <= 0; }

auto round_out(rect area) -> pixel_rect {
  if (area.empty()) {
    return {};
  }
  auto const left = static_cast<int>(std::floor(area.left()));
  auto const top = static_cast<int>(std::floor(area.top()));
  auto const right = static_cast<int>(std::ceil(area.right()));
  auto const bottom = static_cast<int>(std::ceil(area.bottom()));
  return {left, top, right - left, bottom - top};
}

auto to_rect(pixel_rect area) -> rect {
  return {{static_cast<float>(area.x), static_cast<float>(area.y)},
          {static_cast<float>(area.width), static_cast<float>(area.height)}};
}

auto edges(rect area) -> std::array<float, 4> {
  return {area.left(), area.top(), area.right(), area.bottom()};
}

auto premultiplied(basalt::rgba color) -> std::array<float, 4> {
  auto const alpha = static_cast<float>(color.a) / 255;
  return {static_cast<float>(color.r) / 255 * alpha, static_cast<float>(color.g) / 255 * alpha,
          static_cast<float>(color.b) / 255 * alpha, alpha};
}

// The rows of the device-to-local map for a local-to-device transform. Empty when it is
// singular.
auto local_rows(affine_transform const &to_device)
    -> std::optional<std::pair<std::array<float, 4>, std::array<float, 4>>> {
  auto const inverse = basalt::invert(to_device);
  if (!inverse) {
    return std::nullopt;
  }
  return std::pair{std::array{inverse->xx, inverse->xy, inverse->tx, 0.0F},
                   std::array{inverse->yx, inverse->yy, inverse->ty, 0.0F}};
}

constexpr auto identity_x = std::array{1.0F, 0.0F, 0.0F, 0.0F};
constexpr auto identity_y = std::array{0.0F, 1.0F, 0.0F, 0.0F};

// The device pixels a local rect can touch: its transformed bounds and one pixel around them
// for the antialiased edge.
auto device_bounds(affine_transform const &to_device, rect local) -> rect {
  return basalt::outset(basalt::transform_bounds(to_device, local), basalt::insets::uniform(1));
}

enum class entry_kind {
  state,  // Only the transform and clip change.
  layer,  // An offscreen layer.
  culled, // Nothing under it can be seen, so it draws nothing.
};

struct entry {
  entry_kind kind = entry_kind::state;
  affine_transform transform;
  rect clip;
  // For a layer: which one, its opacity, and for a clip that is not axis-aligned, the clip in
  // the local space of `mask_transform`.
  std::uint32_t layer = 0;
  float opacity = 1;
  std::optional<rect> mask;
  affine_transform mask_transform;
};

// The state of one build: the stack of clips, transforms and layers, as in the Blend2D replay.
class recorder {
public:
  recorder(gpu_scene &scene, std::unordered_map<std::uint64_t, std::uint32_t> &image_indexes,
           glyph_atlas &atlas, float scale, rect root_clip)
      : scene_(scene), image_indexes_(image_indexes), atlas_(atlas),
        transform_(affine_transform::scaling(scale, scale)), clip_(root_clip), scale_(scale) {}

  [[nodiscard]] auto drawing() const -> bool { return culled_ == 0; }

  auto finish() -> void {
    while (!stack_.empty()) {
      pop();
    }
  }

  auto push_clip(rect bounds) -> void {
    if (!drawing()) {
      push_culled();
      return;
    }
    auto const device = basalt::intersection(clip_, basalt::transform_bounds(transform_, bounds));
    if (transform_.preserves_axis_alignment()) {
      push_state();
      clip_ = device;
    } else if (is_empty(round_out(device))) {
      push_culled();
    } else {
      push_layer(round_out(device), 1, bounds);
    }
  }

  auto push_transform(affine_transform const &transform) -> void {
    if (!drawing()) {
      push_culled();
      return;
    }
    push_state();
    transform_ = transform_ * transform;
  }

  auto begin_layer(std::uint8_t opacity) -> void {
    if (!drawing() || opacity == 0 || is_empty(round_out(clip_))) {
      push_culled();
    } else if (opacity == 255) {
      // An opaque group looks the same as drawing in place.
      push_state();
    } else {
      push_layer(round_out(clip_), static_cast<float>(opacity) / 255, std::nullopt);
    }
  }

  auto pop() -> void {
    if (stack_.empty()) {
      return;
    }
    auto const top = stack_.back();
    stack_.pop_back();
    transform_ = top.transform;
    clip_ = top.clip;
    switch (top.kind) {
    case entry_kind::culled:
      --culled_;
      break;
    case entry_kind::state:
      break;
    case entry_kind::layer:
      scene_.ops.emplace_back(end_layer_pass{});
      composite(top);
      break;
    }
  }

  auto draw(basalt::paint_command const &command) -> void {
    std::visit(
        [this](auto const &value) {
          using type = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<type, basalt::fill_rect_command>) {
            if (!value.bounds.empty()) {
              add_shape(transform_, value.bounds, 0, std::nullopt, 0, value.color);
            }
          } else if constexpr (std::is_same_v<type, basalt::stroke_rect_command>) {
            auto const half = basalt::insets::uniform(stroke_width(value.width) / 2);
            add_shape(transform_, basalt::outset(value.bounds, half), 0,
                      basalt::inset(value.bounds, half), 0, value.color);
          } else if constexpr (std::is_same_v<type, basalt::fill_rounded_rect_command>) {
            if (!value.bounds.empty()) {
              add_shape(transform_, value.bounds, radius(value.bounds, value.radius), std::nullopt,
                        0, value.color);
            }
          } else if constexpr (std::is_same_v<type, basalt::stroke_rounded_rect_command>) {
            auto const width = stroke_width(value.width);
            auto const half = basalt::insets::uniform(width / 2);
            auto const corner = radius(value.bounds, value.radius);
            add_shape(transform_, basalt::outset(value.bounds, half),
                      corner > 0 ? corner + width / 2 : 0, basalt::inset(value.bounds, half),
                      std::max(corner - width / 2, 0.0F), value.color);
          } else if constexpr (std::is_same_v<type, basalt::line_command>) {
            add_line(value);
          } else if constexpr (std::is_same_v<type, basalt::draw_glyph_run_command>) {
            add_glyph_run(value);
          } else if constexpr (std::is_same_v<type, basalt::draw_image_command>) {
            add_image(value);
          }
        },
        command);
  }

private:
  // Stroke widths of zero draw one device pixel wide, as a hairline.
  [[nodiscard]] auto stroke_width(float width) const -> float {
    return width > 0 ? width : 1 / scale_;
  }

  [[nodiscard]] static auto radius(rect bounds, float radius) -> float {
    return std::clamp(radius, 0.0F, std::min(bounds.width(), bounds.height()) / 2);
  }

  auto push_state() -> void { stack_.push_back({entry_kind::state, transform_, clip_}); }

  auto push_culled() -> void {
    ++culled_;
    stack_.push_back({entry_kind::culled, transform_, clip_});
  }

  auto push_layer(pixel_rect area, float opacity, std::optional<rect> mask) -> void {
    auto const index = static_cast<std::uint32_t>(scene_.layers.size());
    scene_.layers.push_back({area});
    scene_.ops.emplace_back(begin_layer_pass{index});
    stack_.push_back({entry_kind::layer, transform_, clip_, index, opacity, mask, transform_});
    clip_ = to_rect(area);
  }

  // Draws a finished layer into the target below it.
  auto composite(entry const &layer) -> void {
    auto const area = scene_.layers[layer.layer].area;
    auto instance = gpu_instance{};
    instance.inner = {static_cast<float>(area.x), static_cast<float>(area.y),
                      1.0F / static_cast<float>(area.width),
                      1.0F / static_cast<float>(area.height)};
    instance.params = {0, 0, static_cast<float>(instance_kind::layer), 0};
    instance.color = {layer.opacity, layer.opacity, layer.opacity, layer.opacity};
    if (layer.mask) {
      auto const rows = local_rows(layer.mask_transform);
      if (!rows) {
        return;
      }
      instance.outer = edges(*layer.mask);
      std::tie(instance.to_local_x, instance.to_local_y) = *rows;
      instance.bounds = edges(
          basalt::intersection(device_bounds(layer.mask_transform, *layer.mask), to_rect(area)));
    } else {
      instance.outer = edges(to_rect(area));
      instance.to_local_x = identity_x;
      instance.to_local_y = identity_y;
      instance.bounds = edges(to_rect(area));
    }
    add(instance, {texture_ref::source::layer, layer.layer});
  }

  // Appends `instance` with the current clip, unless it is outside the clip.
  auto add(gpu_instance instance, texture_ref texture) -> void {
    auto const bounds =
        basalt::intersection(rect::from_edges(instance.bounds[0], instance.bounds[1],
                                              instance.bounds[2], instance.bounds[3]),
                             clip_);
    if (bounds.empty()) {
      return;
    }
    // Whole pixels, so that every pixel the clip touches gets a fragment.
    instance.bounds = edges(to_rect(round_out(bounds)));
    instance.clip = edges(clip_);
    auto const index = static_cast<std::uint32_t>(scene_.instances.size());
    scene_.instances.push_back(instance);
    if (!scene_.ops.empty()) {
      if (auto *const batch = std::get_if<draw_batch>(&scene_.ops.back());
          batch != nullptr && batch->texture == texture && batch->first + batch->count == index) {
        ++batch->count;
        return;
      }
    }
    scene_.ops.emplace_back(draw_batch{texture, index, 1});
  }

  auto add_shape(affine_transform const &to_device, rect outer, float outer_radius,
                 std::optional<rect> inner, float inner_radius, basalt::rgba color) -> void {
    if (color.a == 0) {
      return;
    }
    auto const rows = local_rows(to_device);
    if (!rows) {
      return;
    }
    auto instance = gpu_instance{};
    instance.bounds = edges(device_bounds(to_device, outer));
    instance.outer = edges(outer);
    if (inner) {
      // A hole of no size still has a position; keep it inside out so that it covers nothing.
      instance.inner = inner->empty() ? std::array{1.0F, 1.0F, 0.0F, 0.0F} : edges(*inner);
    }
    instance.params = {outer_radius, inner_radius, static_cast<float>(instance_kind::shape),
                       inner ? 1.0F : 0.0F};
    instance.color = premultiplied(color);
    std::tie(instance.to_local_x, instance.to_local_y) = *rows;
    add(instance, {});
  }

  // A line is a rect along it, with butt ends.
  auto add_line(basalt::line_command const &line) -> void {
    auto const dx = line.to.x - line.from.x;
    auto const dy = line.to.y - line.from.y;
    auto const length = std::hypot(dx, dy);
    if (length == 0) {
      return;
    }
    auto const along = affine_transform{dx / length, dy / length, -dy / length,
                                        dx / length, line.from.x, line.from.y};
    auto const width = stroke_width(line.width);
    add_shape(transform_ * along, rect::from_edges(0, -width / 2, length, width / 2), 0,
              std::nullopt, 0, line.color);
  }

  auto add_image(basalt::draw_image_command const &command) -> void {
    if (command.image == nullptr || command.bounds.empty()) {
      return;
    }
    auto const rows = local_rows(transform_);
    if (!rows) {
      return;
    }
    auto [found, inserted] = image_indexes_.try_emplace(
        command.image->id(), static_cast<std::uint32_t>(scene_.images.size()));
    if (inserted) {
      scene_.images.push_back(command.image.get());
    }
    auto instance = gpu_instance{};
    instance.bounds = edges(device_bounds(transform_, command.bounds));
    instance.outer = edges(command.bounds);
    instance.inner = {0, 0, 1, 1};
    instance.params = {0, 0, static_cast<float>(instance_kind::image), 0};
    instance.color = {1, 1, 1, 1};
    std::tie(instance.to_local_x, instance.to_local_y) = *rows;
    add(instance, {texture_ref::source::image, found->second});
  }

  auto add_glyph_run(basalt::draw_glyph_run_command const &command) -> void {
    auto const &run = command.run;
    if (run == nullptr || run->face == nullptr || run->glyphs.empty() || command.color.a == 0) {
      return;
    }
    // Glyphs can reach well beyond the advance and the line, for example with accents and
    // swashes. Runs outside the clip are not rasterized.
    auto const reach = basalt::rect::from_edges(
        command.origin.x - run->size, command.origin.y - 2 * run->size,
        command.origin.x + run->advance + run->size, command.origin.y + run->size);
    if (basalt::intersection(clip_, basalt::transform_bounds(transform_, reach)).empty()) {
      return;
    }
    auto const color = premultiplied(command.color);
    auto const page_size = static_cast<float>(atlas_.page_size());
    auto const uv = [page_size](atlas_glyph const &placed) {
      return std::array{static_cast<float>(placed.x) / page_size,
                        static_cast<float>(placed.y) / page_size,
                        static_cast<float>(placed.x + placed.width) / page_size,
                        static_cast<float>(placed.y + placed.height) / page_size};
    };
    auto const kind = static_cast<float>(instance_kind::glyph);

    // Without rotation or stretch, each glyph is placed on whole device pixels, with its
    // subpixel offset rasterized into the bitmap, so the bitmap is copied as it is.
    auto const pixels_per_em = run->size * transform_.xx;
    if (transform_.xy == 0 && transform_.yx == 0 && transform_.xx == transform_.yy &&
        transform_.xx > 0 && pixels_per_em <= glyph_atlas::largest_pixels_per_em) {
      constexpr auto steps = static_cast<float>(glyph_atlas::subpixel_steps);
      for (auto const &glyph : run->glyphs) {
        auto const pen = basalt::apply(transform_, command.origin + glyph.position);
        auto whole_x = std::floor(pen.x);
        auto whole_y = std::floor(pen.y);
        auto step_x = static_cast<int>(std::lround((pen.x - whole_x) * steps));
        auto step_y = static_cast<int>(std::lround((pen.y - whole_y) * steps));
        if (step_x == glyph_atlas::subpixel_steps) {
          step_x = 0;
          whole_x += 1;
        }
        if (step_y == glyph_atlas::subpixel_steps) {
          step_y = 0;
          whole_y += 1;
        }
        auto const *const placed = atlas_.find(*run->face, glyph.id, pixels_per_em, step_x, step_y);
        if (placed == nullptr) {
          continue;
        }
        auto const area = rect{
            {whole_x + static_cast<float>(placed->left), whole_y + static_cast<float>(placed->top)},
            {static_cast<float>(placed->width), static_cast<float>(placed->height)}};
        auto instance = gpu_instance{};
        instance.bounds = edges(area);
        instance.outer = edges(area);
        instance.inner = uv(*placed);
        instance.params = {0, 0, kind, 0};
        instance.color = color;
        instance.to_local_x = identity_x;
        instance.to_local_y = identity_y;
        add(instance, {texture_ref::source::atlas_page, placed->page});
      }
      return;
    }

    // Otherwise each bitmap is drawn as a quad in the run's space and sampled.
    auto const rows = local_rows(transform_);
    if (!rows) {
      return;
    }
    auto const device_scale =
        std::sqrt(std::abs(transform_.xx * transform_.yy - transform_.xy * transform_.yx));
    auto const raster_size = std::min(run->size * device_scale, glyph_atlas::largest_pixels_per_em);
    auto const layout_per_texel = run->size / raster_size;
    for (auto const &glyph : run->glyphs) {
      auto const *const placed = atlas_.find(*run->face, glyph.id, raster_size, 0, 0);
      if (placed == nullptr) {
        continue;
      }
      auto const pen = command.origin + glyph.position;
      auto const area = rect{{pen.x + static_cast<float>(placed->left) * layout_per_texel,
                              pen.y + static_cast<float>(placed->top) * layout_per_texel},
                             {static_cast<float>(placed->width) * layout_per_texel,
                              static_cast<float>(placed->height) * layout_per_texel}};
      auto instance = gpu_instance{};
      instance.bounds = edges(device_bounds(transform_, area));
      instance.outer = edges(area);
      instance.inner = uv(*placed);
      instance.params = {0, 0, kind, 0};
      instance.color = color;
      std::tie(instance.to_local_x, instance.to_local_y) = *rows;
      add(instance, {texture_ref::source::atlas_page, placed->page});
    }
  }

  gpu_scene &scene_;
  std::unordered_map<std::uint64_t, std::uint32_t> &image_indexes_;
  glyph_atlas &atlas_;
  std::vector<entry> stack_;
  // Layout pixels to frame device pixels.
  affine_transform transform_;
  // The current clip in frame device pixels.
  rect clip_;
  float scale_ = 1;
  int culled_ = 0;
};

} // namespace

auto gpu_scene_builder::build(basalt::paint_commands const &commands,
                              std::span<pixel_rect const> areas, float scale, basalt::rgba clear,
                              glyph_atlas &atlas) -> gpu_scene const & {
  scene_.instances.clear();
  scene_.ops.clear();
  scene_.layers.clear();
  scene_.images.clear();
  image_indexes_.clear();

  auto root_clip = rect{};
  for (auto const &area : areas) {
    auto instance = gpu_instance{};
    instance.bounds = edges(to_rect(area));
    instance.outer = instance.bounds;
    instance.params = {0, 0, static_cast<float>(instance_kind::shape), 0};
    instance.color = premultiplied(clear);
    instance.to_local_x = identity_x;
    instance.to_local_y = identity_y;
    instance.clip = instance.bounds;
    scene_.instances.push_back(instance);
    root_clip = basalt::bounding_union(root_clip, to_rect(area));
  }
  scene_.clear_count = scene_.instances.size();

  auto state = recorder{scene_, image_indexes_, atlas, scale, root_clip};
  for (auto const &command : commands) {
    if (auto const *const push_clip = std::get_if<basalt::push_clip_command>(&command)) {
      state.push_clip(push_clip->bounds);
    } else if (auto const *const push_transform =
                   std::get_if<basalt::push_transform_command>(&command)) {
      state.push_transform(push_transform->transform);
    } else if (auto const *const begin_layer = std::get_if<basalt::begin_layer_command>(&command)) {
      state.begin_layer(begin_layer->opacity);
    } else if (std::holds_alternative<basalt::pop_clip_command>(command) ||
               std::holds_alternative<basalt::pop_transform_command>(command) ||
               std::holds_alternative<basalt::end_layer_command>(command)) {
      state.pop();
    } else if (state.drawing()) {
      state.draw(command);
    }
  }
  state.finish();
  return scene_;
}

} // namespace magma::render
