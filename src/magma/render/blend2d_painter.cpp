#include "blend2d_painter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace magma::render {
namespace {

// Entries are never evicted individually. Caches this large mean the caller has moved on
// through many images or fonts, so starting over is cheaper than tracking use.
constexpr auto image_cache_limit = std::size_t{16};
constexpr auto outline_cache_limit = std::size_t{1} << 16;

auto to_bl(basalt::rect bounds) -> BLRect {
  return {bounds.left(), bounds.top(), bounds.width(), bounds.height()};
}

auto to_bl(basalt::rgba color) -> BLRgba32 { return BLRgba32(color.r, color.g, color.b, color.a); }

auto to_bl(basalt::affine_transform const &transform) -> BLMatrix2D {
  return BLMatrix2D(transform.xx, transform.yx, transform.xy, transform.yy, transform.tx,
                    transform.ty);
}

auto to_bl(basalt::point point) -> BLPoint { return {point.x, point.y}; }

auto rounded_rect(basalt::rect bounds, float radius) -> BLRoundRect {
  auto const limit = std::min(bounds.width(), bounds.height()) / 2;
  return {bounds.left(), bounds.top(), bounds.width(), bounds.height(),
          std::clamp(radius, 0.0F, limit)};
}

auto is_empty(BLBoxI box) -> bool { return box.x0 >= box.x1 || box.y0 >= box.y1; }

auto intersect(BLBoxI a, BLBoxI b) -> BLBoxI {
  return {std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
}

auto round_out(basalt::rect area) -> BLBoxI {
  return {static_cast<int>(std::floor(area.left())), static_cast<int>(std::floor(area.top())),
          static_cast<int>(std::ceil(area.right())), static_cast<int>(std::ceil(area.bottom()))};
}

// Bounds that contain everything `command` draws, in layout pixels. They are generous: they only
// let the replay skip commands that are far outside the clip.
auto drawn_bounds(basalt::paint_command const &command) -> basalt::rect {
  return std::visit(
      [](auto const &value) -> basalt::rect {
        using type = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<type, basalt::stroke_rect_command> ||
                      std::is_same_v<type, basalt::stroke_rounded_rect_command>) {
          return basalt::outset(value.bounds, basalt::insets::uniform(value.width / 2 + 1));
        } else if constexpr (std::is_same_v<type, basalt::line_command>) {
          auto const ends = basalt::rect::from_edges(
              std::min(value.from.x, value.to.x), std::min(value.from.y, value.to.y),
              std::max(value.from.x, value.to.x), std::max(value.from.y, value.to.y));
          return basalt::outset(ends, basalt::insets::uniform(value.width / 2 + 1));
        } else if constexpr (std::is_same_v<type, basalt::draw_glyph_run_command>) {
          if (value.run == nullptr) {
            return {};
          }
          // Glyphs can reach well beyond the advance and the line, for example with accents and
          // swashes.
          auto const size = value.run->size;
          return basalt::rect::from_edges(value.origin.x - size, value.origin.y - 2 * size,
                                          value.origin.x + value.run->advance + size,
                                          value.origin.y + size);
        } else if constexpr (requires { value.bounds; }) {
          return value.bounds;
        } else {
          return {};
        }
      },
      command);
}

auto prepare(BLContext &context) -> void {
  context.set_fill_rule(BL_FILL_RULE_NON_ZERO);
  context.set_pattern_quality(BL_PATTERN_QUALITY_BILINEAR);
}

// An offscreen image that collects drawing until it is composited into the one below. It is
// either a group drawn with an opacity, or a clip that is not axis-aligned, drawn through the
// clip's outline.
struct layer {
  BLImage image;
  BLContext context;
  BLPointI origin;
  double opacity = 1;
  // In frame device pixels; empty for a group.
  BLPath mask;
};

enum class entry_kind {
  state,  // A save on the current context.
  layer,  // An offscreen layer.
  culled, // Nothing under it can be seen, so it draws nothing.
};

struct entry {
  entry_kind kind = entry_kind::state;
  basalt::affine_transform transform;
  BLBoxI clip;
};

// The state of one replay: the stack of clips, transforms and layers.
class replay_state final : public replay_target {
public:
  replay_state(BLContext &root, BLBoxI clip, float scale)
      : root_(root), transform_(basalt::affine_transform::scaling(scale, scale)), clip_(clip),
        scale_(scale) {
    root_.set_transform(to_bl(transform_));
  }

  replay_state(replay_state const &) = delete;
  auto operator=(replay_state const &) -> replay_state & = delete;

  ~replay_state() override {
    while (!stack_.empty()) {
      pop();
    }
  }

  [[nodiscard]] auto drawing() const -> bool { return culled_ == 0; }
  [[nodiscard]] auto context() -> BLContext & override {
    return layers_.empty() ? root_ : layers_.back()->context;
  }
  // Stroke widths of zero draw one device pixel wide, as a hairline.
  [[nodiscard]] auto stroke_width(float width) const -> double override {
    return width > 0 ? width : 1.0 / scale_;
  }

  auto push_clip(basalt::rect bounds) -> void {
    if (!drawing()) {
      push_culled();
      return;
    }
    auto const device = intersect(clip_, round_out(basalt::transform_bounds(transform_, bounds)));
    if (transform_.preserves_axis_alignment()) {
      push_state();
      context().clip_to_rect(to_bl(bounds));
      clip_ = device;
    } else if (is_empty(device)) {
      push_culled();
    } else {
      auto mask = BLPath{};
      auto const corners = std::array{
          basalt::point{bounds.left(), bounds.top()},
          basalt::point{bounds.right(), bounds.top()},
          basalt::point{bounds.right(), bounds.bottom()},
          basalt::point{bounds.left(), bounds.bottom()},
      };
      mask.move_to(to_bl(basalt::apply(transform_, corners[0])));
      for (auto i = std::size_t{1}; i < corners.size(); ++i) {
        mask.line_to(to_bl(basalt::apply(transform_, corners[i])));
      }
      mask.close();
      push_layer(device, 1, std::move(mask));
    }
  }

  auto push_transform(basalt::affine_transform const &transform) -> void {
    if (!drawing()) {
      push_culled();
      return;
    }
    push_state();
    transform_ = transform_ * transform;
    context().set_transform(device_transform());
  }

  auto begin_layer(std::uint8_t opacity) -> void {
    if (!drawing() || opacity == 0 || is_empty(clip_)) {
      push_culled();
    } else if (opacity == 255) {
      // An opaque group looks the same as drawing in place.
      push_state();
    } else {
      push_layer(clip_, opacity / 255.0, {});
    }
  }

  [[nodiscard]] auto visible(basalt::rect bounds) const -> bool override {
    return !is_empty(intersect(clip_, round_out(basalt::transform_bounds(transform_, bounds))));
  }

  auto pop() -> void {
    if (stack_.empty()) {
      return;
    }
    auto const top = stack_.back();
    stack_.pop_back();
    switch (top.kind) {
    case entry_kind::culled:
      --culled_;
      break;
    case entry_kind::state:
      context().restore();
      break;
    case entry_kind::layer:
      composite_layer();
      break;
    }
    transform_ = top.transform;
    clip_ = top.clip;
  }

private:
  [[nodiscard]] auto origin() const -> BLPointI {
    return layers_.empty() ? BLPointI{0, 0} : layers_.back()->origin;
  }
  [[nodiscard]] auto device_transform() const -> BLMatrix2D {
    auto const offset = origin();
    return to_bl(basalt::affine_transform::translation(static_cast<float>(-offset.x),
                                                       static_cast<float>(-offset.y)) *
                 transform_);
  }

  auto push_state() -> void {
    context().save();
    stack_.push_back({entry_kind::state, transform_, clip_});
  }

  auto push_culled() -> void {
    ++culled_;
    stack_.push_back({entry_kind::culled, transform_, clip_});
  }

  auto push_layer(BLBoxI box, double opacity, BLPath mask) -> void {
    auto created = std::make_unique<layer>();
    if (created->image.create(box.x1 - box.x0, box.y1 - box.y0, BL_FORMAT_PRGB32) != BL_SUCCESS ||
        created->context.begin(created->image) != BL_SUCCESS) {
      push_culled();
      return;
    }
    created->context.clear_all();
    prepare(created->context);
    created->origin = {box.x0, box.y0};
    created->opacity = opacity;
    created->mask = std::move(mask);
    stack_.push_back({entry_kind::layer, transform_, clip_});
    layers_.push_back(std::move(created));
    clip_ = box;
    context().set_transform(device_transform());
  }

  auto composite_layer() -> void {
    auto finished = std::move(layers_.back());
    layers_.pop_back();
    finished->context.end();

    auto &below = context();
    auto const offset = origin();
    below.save();
    below.set_transform(BLMatrix2D::make_translation(-offset.x, -offset.y));
    if (finished->mask.is_empty()) {
      below.set_global_alpha(finished->opacity);
      below.blit_image(finished->origin, finished->image);
    } else {
      auto const placed = BLMatrix2D::make_translation(finished->origin.x, finished->origin.y);
      below.fill_path(finished->mask, BLPattern(finished->image, BL_EXTEND_MODE_PAD, placed));
    }
    below.restore();
  }

  BLContext &root_;
  std::vector<std::unique_ptr<layer>> layers_;
  std::vector<entry> stack_;
  // Layout pixels to frame device pixels.
  basalt::affine_transform transform_;
  // The device bounds of the current clip.
  BLBoxI clip_;
  float scale_ = 1;
  int culled_ = 0;
};

} // namespace

auto glyph_path(basalt::text::font_face const &face, std::uint32_t glyph) -> BLPath {
  auto path = BLPath{};
  auto const outline = face.outline(glyph, static_cast<float>(face.units_per_em()));
  auto next = outline.points.begin();
  auto const take = [&next] { return to_bl(*next++); };
  for (auto const verb : outline.verbs) {
    switch (verb) {
    case basalt::text::outline_verb::move_to:
      path.move_to(take());
      break;
    case basalt::text::outline_verb::line_to:
      path.line_to(take());
      break;
    case basalt::text::outline_verb::quad_to: {
      auto const control = take();
      path.quad_to(control, take());
      break;
    }
    case basalt::text::outline_verb::cubic_to: {
      auto const first = take();
      auto const second = take();
      path.cubic_to(first, second, take());
      break;
    }
    case basalt::text::outline_verb::close:
      path.close();
      break;
    }
  }
  return path;
}

auto blend2d_painter::image_for(basalt::raster_image const &image) -> BLImage const & {
  if (auto const found = images_.find(image.id()); found != images_.end()) {
    return found->second;
  }
  if (images_.size() >= image_cache_limit) {
    images_.clear();
  }
  auto &copy = images_[image.id()];
  auto const width = static_cast<int>(image.width());
  auto const height = static_cast<int>(image.height());
  auto data = BLImageData{};
  if (copy.create(width, height, BL_FORMAT_PRGB32) != BL_SUCCESS ||
      copy.make_mutable(&data) != BL_SUCCESS) {
    copy.reset();
    return copy;
  }
  auto const &source = image.pixels();
  for (auto y = 0; y < height; ++y) {
    auto *const row = reinterpret_cast<std::uint32_t *>(
        static_cast<std::uint8_t *>(data.pixel_data) + y * data.stride);
    for (auto x = 0; x < width; ++x) {
      auto const *const pixel = &source[(static_cast<std::size_t>(y) * image.width() + x) * 4];
      auto const alpha = std::uint32_t{pixel[3]};
      auto const premultiply = [alpha](std::uint8_t value) {
        return (std::uint32_t{value} * alpha + 127) / 255;
      };
      row[x] = alpha << 24 | premultiply(pixel[0]) << 16 | premultiply(pixel[1]) << 8 |
               premultiply(pixel[2]);
    }
  }
  return copy;
}

auto outline_cache::get(basalt::text::font_face const &face, std::uint32_t glyph)
    -> BLPath const & {
  auto const key = glyph_key{face.id(), glyph};
  if (auto const found = outlines_.find(key); found != outlines_.end()) {
    return found->second;
  }
  if (outlines_.size() >= outline_cache_limit) {
    outlines_.clear();
  }
  return outlines_[key] = glyph_path(face, glyph);
}

auto blend2d_painter::draw(replay_target &target, basalt::paint_command const &command) -> void {
  if (!target.visible(drawn_bounds(command))) {
    return;
  }
  auto &context = target.context();
  // Clips, transforms and layers are handled by the replay, so they draw nothing here.
  std::visit(
      [&]([[maybe_unused]] auto const &value) {
        using type = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<type, basalt::fill_rect_command>) {
          context.fill_rect(to_bl(value.bounds), to_bl(value.color));
        } else if constexpr (std::is_same_v<type, basalt::stroke_rect_command>) {
          context.set_stroke_width(target.stroke_width(value.width));
          context.stroke_rect(to_bl(value.bounds), to_bl(value.color));
        } else if constexpr (std::is_same_v<type, basalt::fill_rounded_rect_command>) {
          context.fill_round_rect(rounded_rect(value.bounds, value.radius), to_bl(value.color));
        } else if constexpr (std::is_same_v<type, basalt::stroke_rounded_rect_command>) {
          context.set_stroke_width(target.stroke_width(value.width));
          context.stroke_round_rect(rounded_rect(value.bounds, value.radius), to_bl(value.color));
        } else if constexpr (std::is_same_v<type, basalt::line_command>) {
          context.set_stroke_width(target.stroke_width(value.width));
          context.stroke_line(to_bl(value.from), to_bl(value.to), to_bl(value.color));
        } else if constexpr (std::is_same_v<type, basalt::draw_glyph_run_command>) {
          draw_glyph_run(context, value);
        } else if constexpr (std::is_same_v<type, basalt::draw_image_command>) {
          if (value.image != nullptr) {
            context.blit_image(to_bl(value.bounds), image_for(*value.image));
          }
        }
      },
      command);
}

auto blend2d_painter::draw_glyph_run(BLContext &context,
                                     basalt::draw_glyph_run_command const &command) -> void {
  auto const &run = command.run;
  if (run == nullptr || run->face == nullptr || run->glyphs.empty()) {
    return;
  }
  // One path for the whole run fills faster than one fill per glyph.
  auto const scale = static_cast<double>(run->size) / run->face->units_per_em();
  run_path_.clear();
  for (auto const &glyph : run->glyphs) {
    auto const &outline = outline_for(*run->face, glyph.id);
    if (!outline.is_empty()) {
      run_path_.add_path(outline,
                         BLMatrix2D(scale, 0, 0, scale, command.origin.x + glyph.position.x,
                                    command.origin.y + glyph.position.y));
    }
  }
  context.fill_path(run_path_, to_bl(command.color));
}

auto blend2d_painter::replay(BLContext &target, BLBoxI clip, float scale,
                             basalt::paint_commands const &commands) -> void {
  prepare(target);
  auto state = replay_state{target, clip, scale};
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
      draw(state, command);
    }
  }
}

} // namespace magma::render
