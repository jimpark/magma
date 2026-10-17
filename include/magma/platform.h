#pragma once

#include "basalt/event.h"
#include "basalt/geometry.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace magma {

using window_id = std::uint32_t;

struct platform_error {
  std::string message;
};

// The drawing API a window is created for. It is fixed when the window is created.
enum class graphics_api {
  software,   // Pixels are presented with `present_pixels`.
  metal,      // `metal_layer` is available (macOS only).
  opengl,     // `create_gl_context` is available.
  vulkan,     // `vulkan_instance_extensions` and `create_vulkan_surface` are available.
  direct3d12, // `win32_window_handle` is available (Windows only). The window keeps its surface.
};

struct window_config {
  graphics_api graphics = graphics_api::software;
  std::string title;
  basalt::size size{960, 640};
  bool resizable = true;
  bool high_pixel_density = true;
};

struct platform_event {
  // A quit request for the whole application uses window 0.
  window_id window = 0;
  basalt::event event;
};

// A rectangle in device pixels.
struct pixel_rect {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

struct file_filter {
  std::string_view name;
  std::string_view pattern;
};

// One refresh of the display a window is on. Times are on the platform's event clock.
struct display_tick {
  std::chrono::nanoseconds time{};     // When the display started to show its latest frame.
  std::chrono::nanoseconds interval{}; // The time until the display's next refresh.
};

enum class cursor_shape {
  arrow,
  text,
  crosshair,
  pointer,
  resize_horizontal,
  resize_vertical,
  wait,
};

// Windows, input, the clipboard, cursors, file dialogs and text input. Events arrive as Basalt
// events. Create one platform per process and use it only on the main thread, except `wake`.
class platform {
public:
  platform(platform const &) = delete;
  auto operator=(platform const &) -> platform & = delete;
  platform(platform &&) noexcept;
  auto operator=(platform &&) noexcept -> platform &;
  ~platform();

  [[nodiscard]] static auto create() -> std::expected<platform, platform_error>;

  [[nodiscard]] auto create_window(window_config const &config)
      -> std::expected<window_id, platform_error>;
  auto destroy_window(window_id window) -> void;
  auto set_window_title(window_id window, std::string_view title) -> void;
  [[nodiscard]] auto window_size(window_id window) const -> std::optional<basalt::size>;
  [[nodiscard]] auto display_scale(window_id window) const -> std::optional<float>;
  // The refresh rate in hertz of the display the window is on, when the system reports one.
  [[nodiscard]] auto display_refresh_rate(window_id window) const -> std::optional<float>;
  // Enters or leaves full screen. The window returns to its earlier size and position.
  auto set_fullscreen(window_id window, bool fullscreen) -> void;
  // The window's position on the desktop, in layout pixels.
  [[nodiscard]] auto window_position(window_id window) const -> std::optional<basalt::point>;
  auto set_window_position(window_id window, basalt::point position) -> void;
  auto set_window_size(window_id window, basalt::size size) -> void;
  // True when the operating system is set to a dark appearance.
  [[nodiscard]] auto system_dark_mode() const -> bool;
  // The drawable size in device pixels (window size times display scale).
  [[nodiscard]] auto pixel_size(window_id window) const -> std::optional<basalt::size>;
  [[nodiscard]] auto poll_events() -> std::vector<platform_event>;
  // The current time on the clock that event times use.
  [[nodiscard]] static auto event_clock_now() -> basalt::event_time;
  // Blocks until an event is queued, `wake` is called or the timeout passes;
  // no timeout waits indefinitely. The events stay queued for `poll_events`.
  auto wait_for_events(std::optional<std::chrono::nanoseconds> timeout) -> void;
  // Ends a wait early. Safe to call while another callback is running on the
  // event thread, such as a file dialog's.
  auto wake() -> void;

  // Asks for one `display_tick` at the next refresh of the display that `window` is on. The tick
  // ends a wait for events, and `take_display_tick` returns it. Ask again for each tick: an idle
  // application gets no ticks. Returns false when the platform cannot follow the display (only
  // macOS 14 and later can), and then the caller paces frames with `display_refresh_rate`.
  auto request_display_tick(window_id window) -> bool;
  // The latest tick for `window` since the previous call, if one arrived.
  [[nodiscard]] auto take_display_tick(window_id window) -> std::optional<display_tick>;

  [[nodiscard]] auto clipboard_text() const -> std::expected<std::string, platform_error>;
  auto set_clipboard_text(std::string_view text) -> std::expected<void, platform_error>;
  auto set_cursor(cursor_shape shape) -> std::expected<void, platform_error>;

  // A missing result means the dialog was cancelled; otherwise it contains the selected paths.
  using file_dialog_callback = std::function<void(std::optional<std::vector<std::string>>)>;
  auto show_open_file_dialog(window_id window, std::span<file_filter const> filters,
                             file_dialog_callback callback, bool allow_many = false,
                             std::string_view default_location = {}) -> void;
  auto show_save_file_dialog(window_id window, std::span<file_filter const> filters,
                             file_dialog_callback callback, std::string_view default_location = {})
      -> void;

  // Copies premultiplied 32-bit BGRA pixels (`row_bytes` apart) to a software
  // window and presents the damaged rectangles; empty damage presents it all.
  // The pixel size must match `pixel_size`.
  auto present_pixels(window_id window, std::span<std::byte const> pixels, int width, int height,
                      std::size_t row_bytes, std::span<pixel_rect const> damage)
      -> std::expected<void, platform_error>;

  // The functions below give a renderer outside Magma access to the window's surface. They
  // become private when Magma's own renderer replaces those renderers.

  // A CAMetalLayer owned by the window, or null when the window is not a Metal
  // window or the platform has no Metal.
  [[nodiscard]] auto metal_layer(window_id window) -> void *;

  // The instance extensions that Vulkan needs to make surfaces for `window`. Empty when the window
  // was not created with `graphics_api::vulkan` or the platform cannot present Vulkan frames.
  [[nodiscard]] auto vulkan_instance_extensions(window_id window) -> std::vector<std::string>;
  // Makes a VkSurfaceKHR for a window created with `graphics_api::vulkan`. `instance` is a
  // VkInstance. Destroy the surface with `destroy_vulkan_surface` before the instance and the
  // window.
  [[nodiscard]] auto create_vulkan_surface(window_id window, void *instance)
      -> std::expected<std::uint64_t, platform_error>;
  auto destroy_vulkan_surface(void *instance, std::uint64_t surface) -> void;
  // The HWND of a window on Windows, or null on other platforms.
  [[nodiscard]] auto win32_window_handle(window_id window) -> void *;

  // Creates the window's OpenGL context and makes it current. Calling it again
  // only makes the existing context current.
  auto create_gl_context(window_id window) -> std::expected<void, platform_error>;
  [[nodiscard]] static auto gl_proc_address(char const *name) -> void *;
  auto swap_gl_buffers(window_id window) -> std::expected<void, platform_error>;

  auto start_text_input(window_id window) -> std::expected<void, platform_error>;
  auto stop_text_input(window_id window) -> std::expected<void, platform_error>;
  auto set_text_input_area(window_id window, basalt::rect area, int cursor_offset = 0)
      -> std::expected<void, platform_error>;

private:
  struct impl;
  explicit platform(std::unique_ptr<impl> implementation);
  std::unique_ptr<impl> impl_;
};

} // namespace magma
