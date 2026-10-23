#pragma once

#include "magma/platform.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_metal.h>
#include <SDL3/SDL_vulkan.h>

#include <exception>
#include <memory>
#include <span>
#include <string>
#include <string_view>
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
  // Files dragged over a window. SDL reports the paths one event at a time and gives a drag that
  // left the window no paths, so they are collected until the drag completes.
  struct file_drag {
    bool entered = false;
    std::vector<std::string> paths;
  };
  std::unordered_map<window_id, file_drag> file_drags;
  live_resize_callback live_resize;
  bool live_resize_watching = false;
  // Kept between frames so that presenting does not allocate.
  std::vector<SDL_Rect> present_rects;
  std::shared_ptr<std::vector<std::exception_ptr>> dialog_callback_errors =
      std::make_shared<std::vector<std::exception_ptr>>();
};

namespace detail {

// What a file or folder dialog needs until SDL calls back. SDL owns the state between the
// call that shows the dialog and `on_dialog`.
struct dialog_state {
  platform::file_dialog_callback callback;
  std::string default_location;
  std::vector<std::string> names;
  std::vector<std::string> patterns;
  std::vector<SDL_DialogFileFilter> filters;
  std::shared_ptr<std::vector<std::exception_ptr>> callback_errors;
};

[[nodiscard]] auto
make_dialog_state(std::span<file_filter const> filters, platform::file_dialog_callback callback,
                  std::string_view default_location,
                  std::shared_ptr<std::vector<std::exception_ptr>> callback_errors)
    -> std::unique_ptr<dialog_state>;
// SDL's dialog callback. It takes ownership of `userdata`, which is a `dialog_state` that
// `make_dialog_state` made and the caller released. A null `filelist` means the user
// cancelled and an empty one means that nothing was chosen. An exception from the callback is
// kept in `callback_errors` for `poll_events` to rethrow, because SDL may call from another thread.
auto on_dialog(void *userdata, char const *const *filelist, int filter) -> void;

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
