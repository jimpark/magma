#include "display_link.h"

namespace magma::detail {

auto make_display_link(SDL_Window *window, std::function<void()> wake)
    -> std::unique_ptr<display_link> {
  (void)window;
  (void)wake;
  return nullptr;
}

} // namespace magma::detail
