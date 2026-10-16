#pragma once

#include "magma/platform.h"

#include <SDL3/SDL.h>

#include <functional>
#include <memory>
#include <optional>

namespace magma::detail {

// Reports the refreshes of the display a window is on. It calls `wake` on the main thread after
// each tick it stores.
class display_link {
public:
  virtual ~display_link() = default;

  // Asks for one tick at the next refresh.
  virtual auto request() -> void = 0;
  [[nodiscard]] virtual auto take() -> std::optional<display_tick> = 0;
};

// A display link for `window`, or null when the platform has none.
[[nodiscard]] auto make_display_link(SDL_Window *window, std::function<void()> wake)
    -> std::unique_ptr<display_link>;

} // namespace magma::detail
