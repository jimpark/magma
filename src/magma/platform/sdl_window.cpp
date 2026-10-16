#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include "sdl_platform_internal.h"

namespace magma {

auto platform::create_window(window_config const &config)
    -> std::expected<window_id, platform_error> {
  auto const width = std::max(1, static_cast<int>(std::lround(config.size.width)));
  auto const height = std::max(1, static_cast<int>(std::lround(config.size.height)));
  auto flags = static_cast<SDL_WindowFlags>(0);
  if (config.resizable) {
    flags |= SDL_WINDOW_RESIZABLE;
  }
  switch (config.graphics) {
  case graphics_api::software:
    break;
  case graphics_api::metal:
    flags |= SDL_WINDOW_METAL;
    break;
  case graphics_api::opengl:
    flags |= SDL_WINDOW_OPENGL;
    break;
  }
  if (config.high_pixel_density) {
    flags |= SDL_WINDOW_HIGH_PIXEL_DENSITY;
  }
  auto *const window = SDL_CreateWindow(config.title.c_str(), width, height, flags);
  if (window == nullptr) {
    return std::unexpected(platform_error{SDL_GetError()});
  }
  auto const id = SDL_GetWindowID(window);
  impl_->windows.emplace(id, sdl_window_handle{window});
  return id;
}

auto platform::destroy_window(window_id window) -> void {
  impl_->metal_views.erase(window);
  impl_->gl_contexts.erase(window);
  impl_->windows.erase(window);
}

auto platform::set_window_title(window_id window, std::string_view title) -> void {
  auto const found = impl_->windows.find(window);
  if (found != impl_->windows.end()) {
    SDL_SetWindowTitle(found->second.get(), std::string(title).c_str());
  }
}

auto platform::set_fullscreen(window_id window, bool fullscreen) -> void {
  auto const found = impl_->windows.find(window);
  if (found != impl_->windows.end()) {
    SDL_SetWindowFullscreen(found->second.get(), fullscreen);
  }
}

auto platform::window_position(window_id window) const -> std::optional<basalt::point> {
  auto const found = impl_->windows.find(window);
  if (found == impl_->windows.end()) {
    return std::nullopt;
  }
  auto x = 0;
  auto y = 0;
  if (!SDL_GetWindowPosition(found->second.get(), &x, &y)) {
    return std::nullopt;
  }
  return basalt::point{static_cast<float>(x), static_cast<float>(y)};
}

auto platform::set_window_position(window_id window, basalt::point position) -> void {
  auto const found = impl_->windows.find(window);
  if (found != impl_->windows.end()) {
    SDL_SetWindowPosition(found->second.get(), static_cast<int>(std::lround(position.x)),
                          static_cast<int>(std::lround(position.y)));
  }
}

auto platform::set_window_size(window_id window, basalt::size size) -> void {
  auto const found = impl_->windows.find(window);
  if (found != impl_->windows.end()) {
    SDL_SetWindowSize(found->second.get(), std::max(1, static_cast<int>(std::lround(size.width))),
                      std::max(1, static_cast<int>(std::lround(size.height))));
  }
}

auto platform::system_dark_mode() const -> bool {
  return SDL_GetSystemTheme() == SDL_SYSTEM_THEME_DARK;
}

auto platform::window_size(window_id window) const -> std::optional<basalt::size> {
  auto const found = impl_->windows.find(window);
  return found == impl_->windows.end() ? std::nullopt
                                       : std::optional{detail::viewport(found->second.get())};
}

auto platform::display_scale(window_id window) const -> std::optional<float> {
  auto const found = impl_->windows.find(window);
  if (found == impl_->windows.end()) {
    return std::nullopt;
  }
  return SDL_GetWindowDisplayScale(found->second.get());
}

auto platform::display_refresh_rate(window_id window) const -> std::optional<float> {
  auto const found = impl_->windows.find(window);
  if (found == impl_->windows.end()) {
    return std::nullopt;
  }
  auto const display = SDL_GetDisplayForWindow(found->second.get());
  auto const *const mode = display == 0 ? nullptr : SDL_GetCurrentDisplayMode(display);
  if (mode == nullptr || mode->refresh_rate <= 0) {
    return std::nullopt;
  }
  return mode->refresh_rate;
}

auto platform::pixel_size(window_id window) const -> std::optional<basalt::size> {
  auto const found = impl_->windows.find(window);
  if (found == impl_->windows.end()) {
    return std::nullopt;
  }
  auto width = 0;
  auto height = 0;
  SDL_GetWindowSizeInPixels(found->second.get(), &width, &height);
  return basalt::size{static_cast<float>(width), static_cast<float>(height)};
}

auto platform::present_pixels(window_id window, std::span<std::byte const> pixels, int width,
                              int height, std::size_t row_bytes, std::span<pixel_rect const> damage)
    -> std::expected<void, platform_error> {
  auto const found = impl_->windows.find(window);
  if (found == impl_->windows.end()) {
    return std::unexpected(platform_error{"unknown window"});
  }
  if (width <= 0 || height <= 0 || pixels.size() < row_bytes * static_cast<std::size_t>(height)) {
    return std::unexpected(platform_error{"pixel buffer is smaller than its dimensions"});
  }
  auto *const target = SDL_GetWindowSurface(found->second.get());
  if (target == nullptr) {
    return std::unexpected(platform_error{SDL_GetError()});
  }
  struct surface_deleter {
    auto operator()(SDL_Surface *surface) const -> void { SDL_DestroySurface(surface); }
  };
  // ARGB8888 is BGRA in memory on little-endian machines.
  auto const source = std::unique_ptr<SDL_Surface, surface_deleter>{
      SDL_CreateSurfaceFrom(width, height, SDL_PIXELFORMAT_ARGB8888,
                            const_cast<std::byte *>(pixels.data()), static_cast<int>(row_bytes))};
  if (source == nullptr) {
    return std::unexpected(platform_error{SDL_GetError()});
  }
  SDL_SetSurfaceBlendMode(source.get(), SDL_BLENDMODE_NONE);

  auto rects = std::vector<SDL_Rect>{};
  for (auto const &area : damage) {
    auto const left = std::clamp(area.x, 0, target->w);
    auto const top = std::clamp(area.y, 0, target->h);
    auto const right = std::clamp(area.x + area.width, 0, target->w);
    auto const bottom = std::clamp(area.y + area.height, 0, target->h);
    if (right > left && bottom > top) {
      rects.push_back({left, top, right - left, bottom - top});
    }
  }
  if (damage.empty()) {
    rects.push_back({0, 0, target->w, target->h});
  }
  for (auto &area : rects) {
    if (!SDL_BlitSurface(source.get(), &area, target, &area)) {
      return std::unexpected(platform_error{SDL_GetError()});
    }
  }
  if (!rects.empty() && !SDL_UpdateWindowSurfaceRects(found->second.get(), rects.data(),
                                                      static_cast<int>(rects.size()))) {
    return std::unexpected(platform_error{SDL_GetError()});
  }
  return {};
}

auto platform::metal_layer(window_id window) -> void * {
  auto const found = impl_->windows.find(window);
  if (found == impl_->windows.end() ||
      (SDL_GetWindowFlags(found->second.get()) & SDL_WINDOW_METAL) == 0) {
    return nullptr;
  }
  auto view = impl_->metal_views.find(window);
  if (view == impl_->metal_views.end()) {
    auto *const created = SDL_Metal_CreateView(found->second.get());
    if (created == nullptr) {
      return nullptr;
    }
    view = impl_->metal_views.emplace(window, sdl_metal_view_handle{created}).first;
  }
  return SDL_Metal_GetLayer(view->second.get());
}

auto platform::create_gl_context(window_id window) -> std::expected<void, platform_error> {
  auto const found = impl_->windows.find(window);
  if (found == impl_->windows.end()) {
    return std::unexpected(platform_error{"unknown window"});
  }
  auto context = impl_->gl_contexts.find(window);
  if (context == impl_->gl_contexts.end()) {
    if ((SDL_GetWindowFlags(found->second.get()) & SDL_WINDOW_OPENGL) == 0) {
      return std::unexpected(platform_error{"window was not created for OpenGL"});
    }
    auto *const created = SDL_GL_CreateContext(found->second.get());
    if (created == nullptr) {
      return std::unexpected(platform_error{SDL_GetError()});
    }
    context = impl_->gl_contexts.emplace(window, sdl_gl_context_handle{created}).first;
  }
  if (!SDL_GL_MakeCurrent(found->second.get(), context->second.get())) {
    return std::unexpected(platform_error{SDL_GetError()});
  }
  // Frames are paced by the renderer, not by the display.
  SDL_GL_SetSwapInterval(0);
  return {};
}

auto platform::gl_proc_address(char const *name) -> void * {
  return reinterpret_cast<void *>(SDL_GL_GetProcAddress(name));
}

auto platform::swap_gl_buffers(window_id window) -> std::expected<void, platform_error> {
  auto const found = impl_->windows.find(window);
  if (found == impl_->windows.end() || !SDL_GL_SwapWindow(found->second.get())) {
    return std::unexpected(
        platform_error{found == impl_->windows.end() ? "unknown window" : SDL_GetError()});
  }
  return {};
}

} // namespace magma
