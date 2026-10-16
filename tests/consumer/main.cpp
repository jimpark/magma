#include "magma/magma.h"

#include <print>

// Opens a window and reads its size back. Exits non-zero unless both work, which needs the SDL
// that Magma links.
auto main() -> int {
  auto platform = magma::platform::create();
  if (!platform) {
    std::println(stderr, "consumer: {}", platform.error().message);
    return 1;
  }
  auto config = magma::window_config{};
  config.size = {320, 200};
  auto const window = platform->create_window(config);
  if (!window) {
    std::println(stderr, "consumer: {}", window.error().message);
    return 1;
  }
  auto const size = platform->window_size(*window);
  std::println("window {}x{}", size ? size->width : 0, size ? size->height : 0);
  return size == basalt::size{320, 200} ? 0 : 1;
}
