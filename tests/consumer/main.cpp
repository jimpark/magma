#include "magma/magma.h"

#include <print>

// Opens a window, reads its size back and renders a frame into it. Exits non-zero unless all of
// it works, which needs the SDL and Blend2D that Magma links.
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
  if (size != basalt::size{320, 200}) {
    return 1;
  }

  auto renderer = magma::renderer::create(*platform, *window);
  if (!renderer) {
    std::println(stderr, "consumer: {}", renderer.error().message);
    return 1;
  }
  constexpr auto green = basalt::rgba{0, 160, 0, 255};
  auto const commands =
      basalt::paint_commands{basalt::fill_rect_command{{{10, 10}, {20, 20}}, green}};
  if (auto const rendered = renderer->render(commands, {}, basalt::rgba{255, 255, 255, 255});
      !rendered) {
    std::println(stderr, "consumer: {}", rendered.error().message);
    return 1;
  }
  return renderer->read_pixel(15, 15) == green ? 0 : 1;
}
