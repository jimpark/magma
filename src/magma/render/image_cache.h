#pragma once

#include "basalt/image.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <list>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "backend.h"

namespace magma::render {

// The GPU copies of the images that scenes sample. Each image is copied once and the copy is kept
// for later frames, so an image that is drawn every frame costs one upload.
//
// The copies of images that stop being drawn are released. The cache holds at most `budget` bytes
// of textures, counted as four bytes per pixel. When the images of a frame push it past the
// budget, the copies that were used longest ago are released first. A copy that the current frame
// samples is never released, so one frame may use more than the budget; the next frame releases
// the excess if it does not sample those images again. A frame that draws only part of the window
// does not release the copies of images outside it until the budget needs the room.
template <class texture> class image_cache {
public:
  explicit image_cache(std::size_t budget) : budget_(budget) {}

  // The textures of `images`, in order. `upload(image)` makes the texture of an image that is not
  // cached yet and returns `std::expected<texture, render_error>`. `retire(texture)` receives
  // every texture the cache releases.
  template <class upload_function, class retire_function>
  [[nodiscard]] auto textures_for(std::span<basalt::raster_image const *const> images,
                                  upload_function &&upload, retire_function &&retire)
      -> std::expected<std::vector<texture>, render_error> {
    ++frame_;
    auto result = std::vector<texture>{};
    result.reserve(images.size());
    for (auto const *const image : images) {
      auto const found = index_.find(image->id());
      if (found != index_.end()) {
        found->second->used = frame_;
        entries_.splice(entries_.begin(), entries_, found->second);
        result.push_back(found->second->value);
        continue;
      }
      auto uploaded = upload(*image);
      if (!uploaded) {
        trim(retire);
        return std::unexpected(std::move(uploaded.error()));
      }
      auto const bytes = std::size_t{image->width()} * image->height() * 4;
      entries_.push_front({image->id(), *uploaded, bytes, frame_});
      index_.emplace(image->id(), entries_.begin());
      held_ += bytes;
      result.push_back(std::move(*uploaded));
    }
    trim(retire);
    return result;
  }

  // Drops every copy without retiring it. The caller releases the device first.
  auto clear() -> void {
    index_.clear();
    entries_.clear();
    held_ = 0;
  }

  [[nodiscard]] auto size() const -> std::size_t { return entries_.size(); }
  [[nodiscard]] auto bytes() const -> std::size_t { return held_; }

private:
  struct entry {
    std::uint64_t id = 0;
    texture value;
    std::size_t bytes = 0;
    std::uint64_t used = 0;
  };

  // Releases the oldest copies until the cache fits its budget, leaving the ones that this frame
  // used. The list is in order of use, so the oldest is last.
  template <class retire_function> auto trim(retire_function &retire) -> void {
    while (held_ > budget_ && !entries_.empty() && entries_.back().used != frame_) {
      auto &oldest = entries_.back();
      held_ -= oldest.bytes;
      index_.erase(oldest.id);
      retire(std::move(oldest.value));
      entries_.pop_back();
    }
  }

  std::size_t budget_;
  std::size_t held_ = 0;
  std::uint64_t frame_ = 0;
  // Most recently used first.
  std::list<entry> entries_;
  std::unordered_map<std::uint64_t, typename std::list<entry>::iterator> index_;
};

} // namespace magma::render
