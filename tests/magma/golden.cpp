#include "golden.h"

#include <blend2d/blend2d.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <string>
#include <string_view>

namespace magma::test {
namespace {

// The largest difference in one channel that still counts as equal.
constexpr auto channel_tolerance = 3;
// The part of the pixels that may differ by more.
constexpr auto outlier_fraction = 0.002;

auto update_requested() -> bool {
#if defined(_MSC_VER)
  char *value = nullptr;
  auto length = std::size_t{0};
  if (_dupenv_s(&value, &length, "MAGMA_UPDATE_GOLDEN") != 0 || value == nullptr) {
    return false;
  }
  auto const requested = std::string_view{value} == "1";
  std::free(value);
  return requested;
#else
  auto const *const value = std::getenv("MAGMA_UPDATE_GOLDEN");
  return value != nullptr && std::string_view{value} == "1";
#endif
}

// Straight RGBA to Blend2D's premultiplied format.
auto to_image(basalt::raster_image const &frame) -> BLImage {
  auto image =
      BLImage{static_cast<int>(frame.width()), static_cast<int>(frame.height()), BL_FORMAT_PRGB32};
  auto data = BLImageData{};
  image.make_mutable(&data);
  auto const &source = frame.pixels();
  for (auto y = std::size_t{0}; y < frame.height(); ++y) {
    auto *const row =
        reinterpret_cast<std::uint32_t *>(static_cast<std::uint8_t *>(data.pixel_data) +
                                          static_cast<std::ptrdiff_t>(y) * data.stride);
    for (auto x = std::size_t{0}; x < frame.width(); ++x) {
      auto const *const pixel = &source[(y * frame.width() + x) * 4];
      auto const alpha = std::uint32_t{pixel[3]};
      auto const premultiply = [alpha](std::uint8_t value) {
        return (std::uint32_t{value} * alpha + 127) / 255;
      };
      row[x] = alpha << 24 | premultiply(pixel[0]) << 16 | premultiply(pixel[1]) << 8 |
               premultiply(pixel[2]);
    }
  }
  return image;
}

auto pixel_at(BLImageData const &data, int x, int y) -> std::uint32_t {
  auto const *const row = static_cast<std::uint8_t const *>(data.pixel_data) + y * data.stride;
  return reinterpret_cast<std::uint32_t const *>(row)[x];
}

auto largest_channel_difference(std::uint32_t a, std::uint32_t b) -> int {
  auto largest = 0;
  for (auto shift = 0; shift < 32; shift += 8) {
    auto const left = static_cast<int>(a >> shift & 0xff);
    auto const right = static_cast<int>(b >> shift & 0xff);
    largest = std::max(largest, left > right ? left - right : right - left);
  }
  return largest;
}

} // namespace

auto compare_with_golden(std::string_view name, basalt::raster_image const &frame)
    -> golden_result {
  auto const golden_path = std::filesystem::path{MAGMA_GOLDEN_DIR} / std::format("{}.png", name);
  auto const actual = to_image(frame);
  if (update_requested()) {
    std::filesystem::create_directories(golden_path.parent_path());
    if (actual.write_to_file(golden_path.string().c_str()) != BL_SUCCESS) {
      return {false, std::format("could not write {}", golden_path.string())};
    }
    return {true, std::format("wrote {}", golden_path.string())};
  }

  auto const output_dir = std::filesystem::path{MAGMA_GOLDEN_OUTPUT_DIR};
  std::filesystem::create_directories(output_dir);
  auto const actual_path = output_dir / std::format("{}.png", name);
  actual.write_to_file(actual_path.string().c_str());

  auto expected = BLImage{};
  if (expected.read_from_file(golden_path.string().c_str()) != BL_SUCCESS) {
    return {false, std::format("{} is missing. The frame is in {}. Run the test with "
                               "MAGMA_UPDATE_GOLDEN=1 to make it the golden image.",
                               golden_path.string(), actual_path.string())};
  }
  expected.convert(BL_FORMAT_PRGB32);
  if (expected.width() != actual.width() || expected.height() != actual.height()) {
    return {false, std::format("{} is {}x{}, but the frame is {}x{}. The frame is in {}.",
                               golden_path.string(), expected.width(), expected.height(),
                               actual.width(), actual.height(), actual_path.string())};
  }

  auto expected_data = BLImageData{};
  auto actual_data = BLImageData{};
  expected.get_data(&expected_data);
  actual.get_data(&actual_data);
  auto differences = BLImage{actual.width(), actual.height(), BL_FORMAT_PRGB32};
  auto difference_data = BLImageData{};
  differences.make_mutable(&difference_data);

  auto outliers = 0;
  auto largest = 0;
  for (auto y = 0; y < actual.height(); ++y) {
    auto *const row = reinterpret_cast<std::uint32_t *>(
        static_cast<std::uint8_t *>(difference_data.pixel_data) + y * difference_data.stride);
    for (auto x = 0; x < actual.width(); ++x) {
      auto const difference =
          largest_channel_difference(pixel_at(expected_data, x, y), pixel_at(actual_data, x, y));
      largest = std::max(largest, difference);
      if (difference > channel_tolerance) {
        ++outliers;
        row[x] = 0xffff0000U;
      } else {
        // A faint copy of the expected pixel shows where the differences are.
        auto const gray = 0xe0U + ((pixel_at(expected_data, x, y) >> 8 & 0xff) >> 3);
        row[x] = 0xff000000U | gray << 16 | gray << 8 | gray;
      }
    }
  }
  auto const allowed =
      static_cast<int>(outlier_fraction * static_cast<double>(actual.width() * actual.height()));
  if (outliers <= allowed) {
    return {true, {}};
  }
  auto const difference_path = output_dir / std::format("{}-diff.png", name);
  differences.write_to_file(difference_path.string().c_str());
  return {false, std::format("{} pixels differ by more than {} (up to {}; {} allowed). The "
                             "frame is in {} and the differences, in red, are in {}.",
                             outliers, channel_tolerance, largest, allowed, actual_path.string(),
                             difference_path.string())};
}

} // namespace magma::test
