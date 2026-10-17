#pragma once

#include "basalt/text/font_face.h"

#include "magma/platform.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

namespace magma::render {

class outline_cache;

// One glyph bitmap in the atlas. `x`, `y`, `width` and `height` are its texels on page `page`.
// `left` and `top` move the pen position, in whole pixels, to the bitmap's top-left corner.
struct atlas_glyph {
  std::uint32_t page = 0;
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  int left = 0;
  int top = 0;
};

// A square of 8-bit coverage, one byte per texel, rows top to bottom. `id` is different for
// every page the atlas makes, so a backend can tell a new page from an old one at the same index.
struct atlas_page {
  std::uint64_t id = 0;
  std::vector<std::uint8_t> pixels;
  // The texels changed since the last `clear_dirty`; empty when none changed.
  pixel_rect dirty;

  struct shelf {
    int y = 0;
    int height = 0;
    int next_x = 0;
  };
  std::vector<shelf> shelves;
  int next_shelf_y = 0;
};

// Glyph coverage for GPU backends. Blend2D rasterizes each glyph once for each size and
// subpixel offset, and the atlas packs the bitmaps into pages that the backend copies to
// textures. All backends share it, so text has the same shape on each of them.
class glyph_atlas {
public:
  // Pen positions are rounded to this fraction of a pixel on each axis.
  static constexpr auto subpixel_steps = 4;
  // Larger glyphs are drawn from a bitmap of this size, scaled up.
  static constexpr auto largest_pixels_per_em = 256.0F;

  explicit glyph_atlas(int page_size = 1024, std::size_t page_limit = 4);
  glyph_atlas(glyph_atlas &&) noexcept;
  auto operator=(glyph_atlas &&) noexcept -> glyph_atlas &;
  ~glyph_atlas();

  [[nodiscard]] auto page_size() const -> int { return page_size_; }

  // The bitmap of `glyph` at `pixels_per_em` device pixels, with the pen `subpixel_x` and
  // `subpixel_y` steps to the right of and below a whole pixel. It is rasterized on first use.
  // Null when the glyph has no coverage, as a space does, or when it does not fit on a page.
  [[nodiscard]] auto find(basalt::text::font_face const &face, std::uint32_t glyph,
                          float pixels_per_em, int subpixel_x, int subpixel_y)
      -> atlas_glyph const *;

  // Starts a frame. When the atlas has more pages than its limit, it drops every glyph and page.
  // Call it only between frames: a reset makes earlier `find` results invalid.
  auto begin_frame() -> void;

  [[nodiscard]] auto pages() const -> std::span<atlas_page const> { return pages_; }
  auto clear_dirty() -> void;

private:
  struct key {
    std::uint64_t face = 0;
    std::uint32_t glyph = 0;
    std::uint32_t pixels_per_em = 0;
    std::uint32_t subpixel = 0;

    [[nodiscard]] friend auto operator==(key const &, key const &) -> bool = default;
  };
  struct key_hash {
    [[nodiscard]] auto operator()(key const &value) const -> std::size_t;
  };

  auto place(int width, int height) -> std::optional<atlas_glyph>;

  int page_size_;
  std::size_t page_limit_;
  std::uint64_t next_page_id_ = 1;
  std::vector<atlas_page> pages_;
  std::unordered_map<key, std::optional<atlas_glyph>, key_hash> glyphs_;
  // Outlines are the same for every size and subpixel step of a glyph.
  std::unique_ptr<outline_cache> outlines_;
};

} // namespace magma::render
