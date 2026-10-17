#include "glyph_atlas.h"

#include <blend2d/blend2d.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

#include "blend2d_painter.h"
#include "pixels.h"

namespace magma::render {
namespace {

// Texels left empty between bitmaps, so that bilinear sampling never reaches a neighbour.
constexpr auto gap = 1;

} // namespace

auto glyph_atlas::key_hash::operator()(key const &value) const -> std::size_t {
  auto hash = value.face * 0x9e3779b97f4a7c15ULL;
  hash ^= (std::uint64_t{value.glyph} << 32 | value.pixels_per_em) + 0x7f4a7c15ULL + (hash << 6);
  hash ^= value.subpixel + (hash >> 2);
  return static_cast<std::size_t>(hash);
}

glyph_atlas::glyph_atlas(int page_size, std::size_t page_limit)
    : page_size_(page_size), page_limit_(page_limit), outlines_(std::make_unique<outline_cache>()) {
}
glyph_atlas::glyph_atlas(glyph_atlas &&) noexcept = default;
auto glyph_atlas::operator=(glyph_atlas &&) noexcept -> glyph_atlas & = default;
glyph_atlas::~glyph_atlas() = default;

auto glyph_atlas::begin_frame() -> void {
  if (pages_.size() > page_limit_) {
    pages_.clear();
    glyphs_.clear();
  }
}

auto glyph_atlas::clear_dirty() -> void {
  for (auto &page : pages_) {
    page.dirty = {};
  }
}

auto glyph_atlas::place(int width, int height) -> std::optional<atlas_glyph> {
  auto const padded_width = width + gap;
  auto const padded_height = height + gap;
  if (padded_width > page_size_ || padded_height > page_size_) {
    return std::nullopt;
  }
  // A shelf is reused only for bitmaps close to its height, so that short glyphs do not waste
  // the space of tall ones.
  auto const fits = [&](atlas_page::shelf const &shelf) {
    return shelf.height >= padded_height && shelf.height <= padded_height + padded_height / 4 + 2 &&
           shelf.next_x + padded_width <= page_size_;
  };
  for (auto index = std::size_t{0}; index < pages_.size(); ++index) {
    auto &page = pages_[index];
    auto const shelf = std::ranges::find_if(page.shelves, fits);
    if (shelf != page.shelves.end()) {
      auto const x = shelf->next_x;
      shelf->next_x += padded_width;
      return atlas_glyph{static_cast<std::uint32_t>(index), x, shelf->y, width, height, 0, 0};
    }
    if (page.next_shelf_y + padded_height <= page_size_) {
      page.shelves.push_back({page.next_shelf_y, padded_height, padded_width});
      auto const y = page.next_shelf_y;
      page.next_shelf_y += padded_height;
      return atlas_glyph{static_cast<std::uint32_t>(index), 0, y, width, height, 0, 0};
    }
  }
  auto &page = pages_.emplace_back();
  page.id = next_page_id_++;
  page.pixels.assign(static_cast<std::size_t>(page_size_) * static_cast<std::size_t>(page_size_),
                     0);
  page.shelves.push_back({0, padded_height, padded_width});
  page.next_shelf_y = padded_height;
  return atlas_glyph{static_cast<std::uint32_t>(pages_.size() - 1), 0, 0, width, height, 0, 0};
}

auto glyph_atlas::find(basalt::text::font_face const &face, std::uint32_t glyph,
                       float pixels_per_em, int subpixel_x, int subpixel_y) -> atlas_glyph const * {
  auto const id = key{face.id(), glyph, std::bit_cast<std::uint32_t>(pixels_per_em),
                      static_cast<std::uint32_t>(subpixel_x * subpixel_steps + subpixel_y)};
  if (auto const found = glyphs_.find(id); found != glyphs_.end()) {
    return found->second ? &*found->second : nullptr;
  }
  auto &entry = glyphs_[id];

  auto const &path = outlines_->get(face, glyph);
  auto box = BLBox{};
  if (path.is_empty() || path.get_bounding_box(&box) != BL_SUCCESS) {
    return nullptr;
  }
  auto const scale = static_cast<double>(pixels_per_em) / face.units_per_em();
  auto const offset_x = static_cast<double>(subpixel_x) / subpixel_steps;
  auto const offset_y = static_cast<double>(subpixel_y) / subpixel_steps;
  // One empty texel on each side keeps the edge coverage inside the bitmap.
  auto const left = static_cast<int>(std::floor(box.x0 * scale + offset_x)) - 1;
  auto const top = static_cast<int>(std::floor(box.y0 * scale + offset_y)) - 1;
  auto const right = static_cast<int>(std::ceil(box.x1 * scale + offset_x)) + 1;
  auto const bottom = static_cast<int>(std::ceil(box.y1 * scale + offset_y)) + 1;
  auto placed = place(right - left, bottom - top);
  if (!placed) {
    return nullptr;
  }
  placed->left = left;
  placed->top = top;

  // Blend2D 0.21 draws wrong coverage into an A8 image when a row is not a multiple of 4 bytes,
  // so the bitmap is made wider and only its first `width` columns are copied.
  auto const padded_width = (placed->width + 15) / 16 * 16;
  auto bitmap = BLImage{};
  auto context = BLContext{};
  if (bitmap.create(padded_width, placed->height, BL_FORMAT_A8) != BL_SUCCESS ||
      context.begin(bitmap) != BL_SUCCESS) {
    return nullptr;
  }
  context.clear_all();
  context.set_fill_rule(BL_FILL_RULE_NON_ZERO);
  context.set_transform(BLMatrix2D(scale, 0, 0, scale, offset_x - left, offset_y - top));
  context.fill_path(path, BLRgba32(0xffffffffU));
  context.end();

  auto data = BLImageData{};
  bitmap.get_data(&data);
  auto &page = pages_[placed->page];
  for (auto row = 0; row < placed->height; ++row) {
    std::memcpy(&page.pixels[static_cast<std::size_t>(placed->y + row) *
                                 static_cast<std::size_t>(page_size_) +
                             static_cast<std::size_t>(placed->x)],
                static_cast<std::uint8_t const *>(data.pixel_data) + row * data.stride,
                static_cast<std::size_t>(placed->width));
  }
  page.dirty = union_of(page.dirty, {placed->x, placed->y, placed->width, placed->height});
  entry = placed;
  return &*entry;
}

} // namespace magma::render
