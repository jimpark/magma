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

// The GPU copies of the images that scenes sample. Each image is copied once and stays until the
// cache holds more than `limit` images; then the least recently drawn images go first. Images
// that the current frame draws are never dropped, so a frame that draws more than `limit` images
// keeps all of them until a later frame needs fewer.
template <class texture> class image_cache {
public:
  explicit image_cache(std::size_t limit) : limit_(limit) {}

  // The textures of `images`, in order. `upload(image)` makes the texture of an image that is not
  // cached yet and returns `std::expected<texture, render_error>`. `retire(texture)` receives
  // every texture that the cache drops.
  template <class upload_function, class retire_function>
  [[nodiscard]] auto textures_for(std::span<basalt::raster_image const *const> images,
                                  upload_function &&upload, retire_function &&retire)
      -> std::expected<std::vector<texture>, render_error> {
    ++frame_;
    auto result = std::vector<texture>{};
    result.reserve(images.size());
    for (auto const *const image : images) {
      auto const id = image->id();
      if (auto const found = cache_.find(id); found != cache_.end()) {
        auto &entry = found->second;
        entry.frame = frame_;
        order_.splice(order_.begin(), order_, entry.position);
        result.push_back(entry.value);
        continue;
      }
      auto uploaded = upload(*image);
      if (!uploaded) {
        return std::unexpected(std::move(uploaded.error()));
      }
      order_.push_front(id);
      auto const &entry =
          cache_.emplace(id, entry_type{std::move(*uploaded), order_.begin(), frame_})
              .first->second;
      result.push_back(entry.value);
    }
    evict(retire);
    return result;
  }

  auto clear() -> void {
    cache_.clear();
    order_.clear();
  }

  [[nodiscard]] auto size() const -> std::size_t { return cache_.size(); }

private:
  struct entry_type {
    texture value;
    std::list<std::uint64_t>::iterator position;
    std::uint64_t frame;
  };

  // `order_` runs from the most to the least recently drawn image, so the images of the current
  // frame are at the front and eviction stops at the first of them.
  template <class retire_function> auto evict(retire_function &retire) -> void {
    while (cache_.size() > limit_ && !order_.empty()) {
      auto const found = cache_.find(order_.back());
      if (found->second.frame == frame_) {
        return;
      }
      retire(std::move(found->second.value));
      cache_.erase(found);
      order_.pop_back();
    }
  }

  std::size_t limit_;
  std::uint64_t frame_ = 0;
  std::list<std::uint64_t> order_;
  std::unordered_map<std::uint64_t, entry_type> cache_;
};

} // namespace magma::render
