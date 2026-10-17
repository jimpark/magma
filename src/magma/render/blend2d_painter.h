#pragma once

#include "basalt/paint.h"

#include <blend2d/blend2d.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>

namespace magma::render {

// The outline of `glyph` in font units, with y growing downward and the face's variation applied.
[[nodiscard]] auto glyph_path(basalt::text::font_face const &face, std::uint32_t glyph) -> BLPath;

// Glyph outlines by face and glyph, made on first use. The cache is not evicted one outline at a
// time. One this large means the caller has moved on through many fonts, so starting over is
// cheaper than tracking use.
class outline_cache {
public:
  [[nodiscard]] auto get(basalt::text::font_face const &face, std::uint32_t glyph)
      -> BLPath const &;

private:
  struct glyph_key {
    std::uint64_t face = 0;
    std::uint32_t glyph = 0;

    [[nodiscard]] friend auto operator==(glyph_key const &, glyph_key const &) -> bool = default;
  };
  struct glyph_key_hash {
    [[nodiscard]] auto operator()(glyph_key const &key) const -> std::size_t {
      return std::hash<std::uint64_t>{}(key.face * 0x9e3779b97f4a7c15ULL ^ key.glyph);
    }
  };

  std::unordered_map<glyph_key, BLPath, glyph_key_hash> outlines_;
};

// Where the painter draws during a replay: the current context, which changes when a layer
// opens or closes.
class replay_target {
public:
  virtual ~replay_target() = default;
  [[nodiscard]] virtual auto context() -> BLContext & = 0;
  [[nodiscard]] virtual auto stroke_width(float width) const -> double = 0;
  // False when `bounds`, in layout pixels, cannot reach the current clip.
  [[nodiscard]] virtual auto visible(basalt::rect bounds) const -> bool = 0;
};

// Replays Basalt paint commands into a Blend2D context. It keeps device copies of images and
// glyph outlines between frames, so one painter serves many frames.
class blend2d_painter {
public:
  // Draws `commands` into `target`, which draws into an image of device pixels. `clip` is the
  // device area the caller has clipped `target` to. Commands are in layout pixels and are
  // scaled by `scale`. Pops without a matching push are ignored, and unclosed pushes are
  // closed at the end.
  auto replay(BLContext &target, BLBoxI clip, float scale, basalt::paint_commands const &commands)
      -> void;

  // The Blend2D copy of `image`, premultiplied, made on first use.
  [[nodiscard]] auto image_for(basalt::raster_image const &image) -> BLImage const &;
  // The outline of `glyph` in font units, with y growing downward.
  [[nodiscard]] auto outline_for(basalt::text::font_face const &face, std::uint32_t glyph)
      -> BLPath const & {
    return outlines_.get(face, glyph);
  }

private:
  auto draw(replay_target &target, basalt::paint_command const &command) -> void;
  auto draw_glyph_run(BLContext &context, basalt::draw_glyph_run_command const &command) -> void;

  std::unordered_map<std::uint64_t, BLImage> images_;
  outline_cache outlines_;
  BLPath run_path_;
};

} // namespace magma::render
