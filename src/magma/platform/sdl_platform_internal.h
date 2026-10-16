#pragma once

#include "magma/platform.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>

#include <exception>
#include <memory>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "display_link.h"

namespace magma {

struct sdl_window_deleter {
  auto operator()(SDL_Window *window) const -> void { SDL_DestroyWindow(window); }
};

struct sdl_cursor_deleter {
  auto operator()(SDL_Cursor *cursor) const -> void { SDL_DestroyCursor(cursor); }
};

struct sdl_gl_context_deleter {
  auto operator()(SDL_GLContext context) const -> void { SDL_GL_DestroyContext(context); }
};

struct sdl_metal_view_deleter {
  auto operator()(void *view) const -> void { SDL_Metal_DestroyView(view); }
};

using sdl_gl_context_handle =
    std::unique_ptr<std::remove_pointer_t<SDL_GLContext>, sdl_gl_context_deleter>;
using sdl_metal_view_handle = std::unique_ptr<void, sdl_metal_view_deleter>;
using sdl_window_handle = std::unique_ptr<SDL_Window, sdl_window_deleter>;
using sdl_cursor_handle = std::unique_ptr<SDL_Cursor, sdl_cursor_deleter>;

struct platform::impl {
  std::unordered_map<window_id, sdl_window_handle> windows;
  // Per-window graphics resources; they must be released before the window.
  std::unordered_map<window_id, sdl_metal_view_handle> metal_views;
  std::unordered_map<window_id, sdl_gl_context_handle> gl_contexts;
  // Created on the first request. A null entry means the window cannot have one.
  std::unordered_map<window_id, std::unique_ptr<detail::display_link>> display_links;
  auto release_graphics() -> void {
    display_links.clear();
    metal_views.clear();
    gl_contexts.clear();
  }
  std::unordered_map<cursor_shape, sdl_cursor_handle> cursors;
  std::shared_ptr<std::vector<std::exception_ptr>> dialog_callback_errors =
      std::make_shared<std::vector<std::exception_ptr>>();
};

namespace detail {

// Pushes an event that ends `platform::wait_for_events`. Safe on any thread.
inline auto push_wake_event() -> void {
  auto event = SDL_Event{};
  event.type = SDL_EVENT_USER;
  SDL_PushEvent(&event);
}

inline auto window_for(SDL_WindowID id) -> SDL_Window * { return SDL_GetWindowFromID(id); }

inline auto viewport(SDL_Window *window) -> basalt::size {
  auto width = 0;
  auto height = 0;
  SDL_GetWindowSize(window, &width, &height);
  return {static_cast<float>(width), static_cast<float>(height)};
}

} // namespace detail
} // namespace magma
