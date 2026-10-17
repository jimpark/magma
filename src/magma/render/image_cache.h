#pragma once

#include "basalt/image.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <unordered_map>
#include <utility>
#include <vector>

#include "backend.h"

namespace magma::render {

// The GPU copies of the images that scenes sample. Each image is copied once. Entries are not
// evicted one at a time: a cache this large means the caller has moved on through many images, so
// starting over is cheaper than tracking use.
template <class texture> class image_cache {
public:
  explicit image_cache(std::size_t limit) : limit_(limit) {}

  // The textures of `images`, in order. `upload(image)` makes the texture of an image that is not
  // cached yet and returns `std::expected<texture, render_error>`. `retire(texture)` receives
  // every texture dropped when the cache starts over.
  template <class upload_function, class retire_function>
  [[nodiscard]] auto textures_for(std::span<basalt::raster_image const *const> images,
                                  upload_function &&upload, retire_function &&retire)
      -> std::expected<std::vector<texture>, render_error> {
    if (cache_.size() + images.size() > limit_) {
      for (auto &entry : cache_) {
        retire(std::move(entry.second));
      }
      cache_.clear();
    }
    auto result = std::vector<texture>{};
    result.reserve(images.size());
    for (auto const *const image : images) {
      auto &cached = cache_[image->id()];
      if (!cached) {
        auto uploaded = upload(*image);
        if (!uploaded) {
          cache_.erase(image->id());
          return std::unexpected(std::move(uploaded.error()));
        }
        cached = std::move(*uploaded);
      }
      result.push_back(cached);
    }
    return result;
  }

  auto clear() -> void { cache_.clear(); }

private:
  std::size_t limit_;
  std::unordered_map<std::uint64_t, texture> cache_;
};

} // namespace magma::render
