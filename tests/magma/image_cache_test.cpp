#include <catch2/catch_test_macros.hpp>

#include "basalt/image.h"

#include "magma/render/image_cache.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <vector>

// The cache is generic over the texture, so these tests use an integer for it and count the
// uploads and the releases.

namespace {

using magma::render::image_cache;

// A 16x16 image takes 1,024 bytes.
auto make_image() -> basalt::raster_image_ptr {
  return basalt::raster_image::create(16, 16, std::vector<std::uint8_t>(16 * 16 * 4, 1));
}

struct recorder {
  int uploads = 0;
  std::vector<int> retired;
  int next = 0;

  auto upload() {
    return [this](basalt::raster_image const &) -> std::expected<int, magma::render_error> {
      ++uploads;
      return ++next;
    };
  }
  auto retire() {
    return [this](int texture) { retired.push_back(texture); };
  }
};

auto pointers(std::vector<basalt::raster_image_ptr> const &images)
    -> std::vector<basalt::raster_image const *> {
  auto result = std::vector<basalt::raster_image const *>{};
  for (auto const &image : images) {
    result.push_back(image.get());
  }
  return result;
}

auto make_images(std::size_t count) -> std::vector<basalt::raster_image_ptr> {
  auto result = std::vector<basalt::raster_image_ptr>{};
  for (std::size_t i = 0; i < count; ++i) {
    result.push_back(make_image());
  }
  return result;
}

} // namespace

TEST_CASE("an image that is drawn every frame is uploaded once", "[image_cache]") {
  auto cache = image_cache<int>{1'000'000};
  auto log = recorder{};
  auto const images = make_images(3);
  auto const scene = pointers(images);
  auto const first = cache.textures_for(scene, log.upload(), log.retire());
  auto const second = cache.textures_for(scene, log.upload(), log.retire());
  REQUIRE(first.has_value());
  REQUIRE(second.has_value());
  CHECK(*first == std::vector<int>{1, 2, 3});
  CHECK(*second == *first);
  CHECK(log.uploads == 3);
  CHECK(log.retired.empty());
}

TEST_CASE("textures come back in the order of the scene's images", "[image_cache]") {
  auto cache = image_cache<int>{1'000'000};
  auto log = recorder{};
  auto const images = make_images(3);
  auto scene = pointers(images);
  REQUIRE(cache.textures_for(scene, log.upload(), log.retire()).has_value());
  std::swap(scene[0], scene[2]);
  auto const again = cache.textures_for(scene, log.upload(), log.retire());
  CHECK(*again == std::vector<int>{3, 2, 1});
}

TEST_CASE("images that are no longer drawn are released when the budget is full", "[image_cache]") {
  // The budget holds four images.
  auto cache = image_cache<int>{4 * 1'024};
  auto log = recorder{};
  auto const old_images = make_images(4);
  REQUIRE(cache.textures_for(pointers(old_images), log.upload(), log.retire()).has_value());
  CHECK(cache.bytes() == 4 * 1'024);
  CHECK(log.retired.empty());

  auto const new_images = make_images(3);
  REQUIRE(cache.textures_for(pointers(new_images), log.upload(), log.retire()).has_value());
  // Three of the four old copies went, oldest first.
  CHECK(log.retired == std::vector<int>{1, 2, 3});
  CHECK(cache.size() == 4);
  CHECK(cache.bytes() == 4 * 1'024);
}

TEST_CASE("the copy used longest ago goes first", "[image_cache]") {
  auto cache = image_cache<int>{3 * 1'024};
  auto log = recorder{};
  auto const images = make_images(4);
  auto const a = pointers({images[0], images[1], images[2]});
  REQUIRE(cache.textures_for(a, log.upload(), log.retire()).has_value());
  // Draw the first image again, so that the second is the oldest.
  auto const again = pointers({images[0]});
  REQUIRE(cache.textures_for(again, log.upload(), log.retire()).has_value());
  auto const fourth = pointers({images[3]});
  REQUIRE(cache.textures_for(fourth, log.upload(), log.retire()).has_value());
  CHECK(log.retired == std::vector<int>{2});
}

TEST_CASE("a frame keeps every image it draws, even past the budget", "[image_cache]") {
  auto cache = image_cache<int>{2 * 1'024};
  auto log = recorder{};
  auto const images = make_images(5);
  auto const result = cache.textures_for(pointers(images), log.upload(), log.retire());
  REQUIRE(result.has_value());
  CHECK(result->size() == 5);
  CHECK(log.retired.empty());
  CHECK(cache.size() == 5);
  // The next frame draws one image and the cache shrinks to its budget.
  auto const one = pointers({images[4]});
  REQUIRE(cache.textures_for(one, log.upload(), log.retire()).has_value());
  CHECK(cache.size() == 2);
  CHECK(log.retired.size() == 3);
}

TEST_CASE("a released image is uploaded again when it comes back", "[image_cache]") {
  auto cache = image_cache<int>{1'024};
  auto log = recorder{};
  auto const images = make_images(2);
  REQUIRE(cache.textures_for(pointers({images[0]}), log.upload(), log.retire()).has_value());
  REQUIRE(cache.textures_for(pointers({images[1]}), log.upload(), log.retire()).has_value());
  REQUIRE(cache.textures_for(pointers({images[0]}), log.upload(), log.retire()).has_value());
  CHECK(log.uploads == 3);
  CHECK(log.retired == std::vector<int>{1, 2});
}

TEST_CASE("scrolling through many images keeps the cache within its budget", "[image_cache]") {
  auto cache = image_cache<int>{50 * 1'024};
  auto log = recorder{};
  auto const images = make_images(2'000);
  for (std::size_t first = 0; first + 40 <= images.size(); first += 4) {
    auto const window = std::vector<basalt::raster_image_ptr>(
        images.begin() + static_cast<std::ptrdiff_t>(first),
        images.begin() + static_cast<std::ptrdiff_t>(first + 40));
    REQUIRE(cache.textures_for(pointers(window), log.upload(), log.retire()).has_value());
    CHECK(cache.bytes() <= 50 * 1'024);
  }
  // Each image is uploaded once on the way down.
  CHECK(log.uploads == 2'000);
  CHECK(cache.size() == 50);
}

TEST_CASE("a failed upload is reported and the cache stays usable", "[image_cache]") {
  auto cache = image_cache<int>{1'000'000};
  auto log = recorder{};
  auto const images = make_images(2);
  auto const failure = cache.textures_for(
      pointers(images),
      [&](basalt::raster_image const &) -> std::expected<int, magma::render_error> {
        return std::unexpected(magma::render_error{"out of memory"});
      },
      log.retire());
  REQUIRE_FALSE(failure.has_value());
  CHECK(failure.error().message == "out of memory");
  auto const retry = cache.textures_for(pointers(images), log.upload(), log.retire());
  REQUIRE(retry.has_value());
  CHECK(retry->size() == 2);
}

TEST_CASE("clear forgets every copy without retiring it", "[image_cache]") {
  auto cache = image_cache<int>{1'000'000};
  auto log = recorder{};
  auto const images = make_images(2);
  REQUIRE(cache.textures_for(pointers(images), log.upload(), log.retire()).has_value());
  cache.clear();
  CHECK(cache.size() == 0);
  CHECK(cache.bytes() == 0);
  CHECK(log.retired.empty());
}
