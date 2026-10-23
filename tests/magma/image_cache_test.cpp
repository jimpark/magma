#include <catch2/catch_test_macros.hpp>

#include "basalt/image.h"

#include "magma/render/image_cache.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <tuple>
#include <vector>

namespace {

using cache = magma::render::image_cache<std::uint64_t>;

auto make_images(std::size_t count) -> std::vector<basalt::raster_image_ptr> {
  auto images = std::vector<basalt::raster_image_ptr>{};
  for (auto index = std::size_t{0}; index < count; ++index) {
    images.push_back(basalt::raster_image::create(1, 1, {0, 0, 0, 255}));
  }
  return images;
}

auto pointers(std::span<basalt::raster_image_ptr const> images)
    -> std::vector<basalt::raster_image const *> {
  auto result = std::vector<basalt::raster_image const *>{};
  for (auto const &image : images) {
    result.push_back(image.get());
  }
  return result;
}

struct counts {
  std::size_t uploads = 0;
  std::vector<std::uint64_t> retired;
};

// Draws `images` as one frame. A texture is the id of its image.
auto draw(cache &images, std::span<basalt::raster_image_ptr const> frame, counts &seen)
    -> std::vector<std::uint64_t> {
  auto const drawn = pointers(frame);
  auto textures = images.textures_for(
      drawn,
      [&](basalt::raster_image const &image) -> std::expected<std::uint64_t, magma::render_error> {
        ++seen.uploads;
        return image.id();
      },
      [&](std::uint64_t texture) { seen.retired.push_back(texture); });
  REQUIRE(textures.has_value());
  return *textures;
}

} // namespace

TEST_CASE("the same images are uploaded once while they fit", "[image_cache]") {
  auto images = cache{64};
  auto const frame = make_images(64);
  auto seen = counts{};
  for (auto repeat = 0; repeat < 10; ++repeat) {
    auto const textures = draw(images, frame, seen);
    REQUIRE(textures.size() == frame.size());
    CHECK(textures.front() == frame.front()->id());
  }
  CHECK(seen.uploads == 64);
  CHECK(seen.retired.empty());
}

TEST_CASE("a frame with more images than the limit keeps all of them", "[image_cache]") {
  auto images = cache{8};
  auto const frame = make_images(20);
  auto seen = counts{};
  for (auto repeat = 0; repeat < 3; ++repeat) {
    std::ignore = draw(images, frame, seen);
  }
  CHECK(seen.uploads == 20);
  CHECK(seen.retired.empty());
  CHECK(images.size() == 20);
}

TEST_CASE("the least recently drawn images are dropped first", "[image_cache]") {
  auto images = cache{4};
  auto const all = make_images(6);
  auto seen = counts{};
  std::ignore = draw(images, std::span{all}.subspan(0, 4), seen);
  // Draw image 0 again, so images 1 and 2 are now the least recently drawn.
  std::ignore = draw(images, std::span{all}.subspan(0, 1), seen);
  std::ignore = draw(images, std::span{all}.subspan(4, 2), seen);

  CHECK(seen.uploads == 6);
  CHECK(seen.retired == std::vector<std::uint64_t>{all[1]->id(), all[2]->id()});
  CHECK(images.size() == 4);

  seen = counts{};
  std::ignore = draw(images, std::span{all}.subspan(0, 1), seen);
  CHECK(seen.uploads == 0);
}

TEST_CASE("a failed upload is not cached", "[image_cache]") {
  auto images = cache{4};
  auto const frame = make_images(1);
  auto const drawn = pointers(frame);
  auto const failed = images.textures_for(
      drawn,
      [](basalt::raster_image const &) -> std::expected<std::uint64_t, magma::render_error> {
        return std::unexpected(magma::render_error{});
      },
      [](std::uint64_t) {});
  CHECK_FALSE(failed.has_value());
  CHECK(images.size() == 0);
}
