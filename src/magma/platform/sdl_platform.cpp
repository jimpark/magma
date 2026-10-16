#include "magma/platform.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

#include "sdl_platform_internal.h"

namespace magma {
namespace {

constexpr auto wheel_notch_layout_pixels = 40.0f;

auto now() -> basalt::event_time {
  return std::chrono::nanoseconds{static_cast<std::int64_t>(SDL_GetTicksNS())};
}

auto event_time(Uint64 timestamp) -> basalt::event_time {
  return std::chrono::nanoseconds{static_cast<std::int64_t>(timestamp)};
}

auto modifiers(SDL_Keymod value) -> basalt::modifier {
  auto result = basalt::modifier::none;
  if ((value & SDL_KMOD_SHIFT) != 0) {
    result |= basalt::modifier::shift;
  }
  if ((value & SDL_KMOD_CTRL) != 0) {
    result |= basalt::modifier::control;
  }
  if ((value & SDL_KMOD_ALT) != 0) {
    result |= basalt::modifier::alt;
  }
  if ((value & SDL_KMOD_GUI) != 0) {
    result |= basalt::modifier::meta;
  }
  return result;
}

auto translate_key(SDL_Keycode value) -> basalt::key {
  if (value >= SDLK_A && value <= SDLK_Z) {
    return static_cast<basalt::key>(static_cast<int>(basalt::key::a) + value - SDLK_A);
  }
  if (value >= SDLK_0 && value <= SDLK_9) {
    return static_cast<basalt::key>(static_cast<int>(basalt::key::digit_0) + value - SDLK_0);
  }
  if (value >= SDLK_F1 && value <= SDLK_F12) {
    return static_cast<basalt::key>(static_cast<int>(basalt::key::f1) + value - SDLK_F1);
  }
  switch (value) {
  case SDLK_LEFT:
    return basalt::key::left;
  case SDLK_RIGHT:
    return basalt::key::right;
  case SDLK_UP:
    return basalt::key::up;
  case SDLK_DOWN:
    return basalt::key::down;
  case SDLK_HOME:
    return basalt::key::home;
  case SDLK_END:
    return basalt::key::end;
  case SDLK_PAGEUP:
    return basalt::key::page_up;
  case SDLK_PAGEDOWN:
    return basalt::key::page_down;
  case SDLK_RETURN:
  case SDLK_KP_ENTER:
    return basalt::key::enter;
  case SDLK_ESCAPE:
    return basalt::key::escape;
  case SDLK_TAB:
    return basalt::key::tab;
  case SDLK_SPACE:
    return basalt::key::space;
  case SDLK_BACKSPACE:
    return basalt::key::backspace;
  case SDLK_DELETE:
    return basalt::key::delete_forward;
  case SDLK_INSERT:
    return basalt::key::insert;
  case SDLK_MINUS:
    return basalt::key::minus;
  case SDLK_EQUALS:
    return basalt::key::equal;
  case SDLK_LEFTBRACKET:
    return basalt::key::left_bracket;
  case SDLK_RIGHTBRACKET:
    return basalt::key::right_bracket;
  case SDLK_BACKSLASH:
    return basalt::key::backslash;
  case SDLK_SEMICOLON:
    return basalt::key::semicolon;
  case SDLK_APOSTROPHE:
    return basalt::key::quote;
  case SDLK_GRAVE:
    return basalt::key::backquote;
  case SDLK_COMMA:
    return basalt::key::comma;
  case SDLK_PERIOD:
    return basalt::key::period;
  case SDLK_SLASH:
    return basalt::key::slash;
  case SDLK_LSHIFT:
    return basalt::key::left_shift;
  case SDLK_RSHIFT:
    return basalt::key::right_shift;
  case SDLK_LCTRL:
    return basalt::key::left_control;
  case SDLK_RCTRL:
    return basalt::key::right_control;
  case SDLK_LALT:
    return basalt::key::left_alt;
  case SDLK_RALT:
    return basalt::key::right_alt;
  case SDLK_LGUI:
    return basalt::key::left_meta;
  case SDLK_RGUI:
    return basalt::key::right_meta;
  case SDLK_CAPSLOCK:
    return basalt::key::caps_lock;
  case SDLK_MENU:
    return basalt::key::menu;
  default:
    return basalt::key::unknown;
  }
}

auto translate_button(Uint8 value) -> basalt::pointer_button {
  switch (value) {
  case SDL_BUTTON_LEFT:
    return basalt::pointer_button::primary;
  case SDL_BUTTON_RIGHT:
    return basalt::pointer_button::secondary;
  case SDL_BUTTON_MIDDLE:
    return basalt::pointer_button::middle;
  case SDL_BUTTON_X1:
    return basalt::pointer_button::back;
  case SDL_BUTTON_X2:
    return basalt::pointer_button::forward;
  default:
    return basalt::pointer_button::none;
  }
}

auto translate_buttons(SDL_MouseButtonFlags value) -> basalt::pointer_button {
  auto result = basalt::pointer_button::none;
  if ((value & SDL_BUTTON_LMASK) != 0) {
    result |= basalt::pointer_button::primary;
  }
  if ((value & SDL_BUTTON_RMASK) != 0) {
    result |= basalt::pointer_button::secondary;
  }
  if ((value & SDL_BUTTON_MMASK) != 0) {
    result |= basalt::pointer_button::middle;
  }
  if ((value & SDL_BUTTON_X1MASK) != 0) {
    result |= basalt::pointer_button::back;
  }
  if ((value & SDL_BUTTON_X2MASK) != 0) {
    result |= basalt::pointer_button::forward;
  }
  return result;
}

auto utf8_offset(std::string_view text, std::size_t characters) -> std::size_t {
  auto offset = std::size_t{0};
  while (characters > 0 && offset < text.size()) {
    auto const lead = static_cast<unsigned char>(text[offset]);
    auto const length = lead < 0x80 ? 1U : lead < 0xe0 ? 2U : lead < 0xf0 ? 3U : 4U;
    offset = std::min(text.size(), offset + length);
    --characters;
  }
  return offset;
}

auto dialog_filters(std::span<file_filter const> filters, std::vector<std::string> &names,
                    std::vector<std::string> &patterns) -> std::vector<SDL_DialogFileFilter> {
  names.reserve(filters.size());
  patterns.reserve(filters.size());
  for (auto const &filter : filters) {
    names.emplace_back(filter.name);
    patterns.emplace_back(filter.pattern);
  }
  auto result = std::vector<SDL_DialogFileFilter>{};
  result.reserve(filters.size());
  for (auto index = std::size_t{0}; index < filters.size(); ++index) {
    result.push_back({names[index].c_str(), patterns[index].c_str()});
  }
  return result;
}

struct dialog_state {
  platform::file_dialog_callback callback;
  std::string default_location;
  std::vector<std::string> names;
  std::vector<std::string> patterns;
  std::vector<SDL_DialogFileFilter> filters;
  std::shared_ptr<std::vector<std::exception_ptr>> callback_errors;
};

auto make_dialog_state(std::span<file_filter const> filters,
                       platform::file_dialog_callback callback, std::string_view default_location,
                       std::shared_ptr<std::vector<std::exception_ptr>> callback_errors)
    -> std::unique_ptr<dialog_state> {
  auto state = std::make_unique<dialog_state>();
  state->callback = std::move(callback);
  state->default_location = default_location;
  state->filters = dialog_filters(filters, state->names, state->patterns);
  state->callback_errors = std::move(callback_errors);
  return state;
}

auto pointer_position(float x, float y) -> basalt::point { return {x, y}; }

auto append_window_event(std::vector<platform_event> &result, SDL_WindowID id,
                         basalt::window_action action, basalt::event_time timestamp) -> void {
  auto value = basalt::window_event{};
  value.action = action;
  value.time = timestamp;
  if (action == basalt::window_action::resized) {
    if (auto *window = detail::window_for(id); window != nullptr) {
      value.viewport = detail::viewport(window);
    }
  } else if (action == basalt::window_action::scale_changed) {
    if (auto *window = detail::window_for(id); window != nullptr) {
      value.display_scale = SDL_GetWindowDisplayScale(window);
    }
  }
  result.push_back({id, value});
}

auto on_dialog(void *userdata, char const *const *filelist, int) -> void {
  auto state = std::unique_ptr<dialog_state>{static_cast<dialog_state *>(userdata)};
  std::optional<std::vector<std::string>> paths;
  if (filelist != nullptr) {
    paths.emplace();
    for (auto index = std::size_t{0}; filelist[index] != nullptr; ++index) {
      paths->emplace_back(filelist[index]);
    }
  }
  try {
    state->callback(paths);
  } catch (...) {
    state->callback_errors->push_back(std::current_exception());
  }
}

} // namespace

platform::platform(std::unique_ptr<impl> implementation) : impl_(std::move(implementation)) {}
platform::platform(platform &&) noexcept = default;
auto platform::operator=(platform &&other) noexcept -> platform & {
  if (this != &other) {
    if (impl_ != nullptr) {
      impl_->release_graphics();
      impl_->windows.clear();
      impl_->cursors.clear();
      SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
    }
    impl_ = std::move(other.impl_);
  }
  return *this;
}

platform::~platform() {
  if (impl_ != nullptr) {
    impl_->release_graphics();
    impl_->windows.clear();
    impl_->cursors.clear();
    SDL_QuitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS);
  }
}

auto platform::create() -> std::expected<platform, platform_error> {
  // The click that activates a window should also reach the widget under it.
  SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
  if (!SDL_InitSubSystem(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
    return std::unexpected(platform_error{SDL_GetError()});
  }
  return platform{std::make_unique<impl>()};
}

auto platform::wait_for_events(std::optional<std::chrono::nanoseconds> timeout) -> void {
  // A null event pointer leaves the event queued.
  if (!timeout) {
    SDL_WaitEvent(nullptr);
    return;
  }
  auto const deadline = now() + *timeout;
  auto const milliseconds = std::chrono::floor<std::chrono::milliseconds>(
      std::max(*timeout, std::chrono::nanoseconds{0}));
  if (SDL_WaitEventTimeout(nullptr, static_cast<Sint32>(milliseconds.count()))) {
    return;
  }
  // SDL waits in whole milliseconds. A short sleep ends the wait at the exact time; a timer on
  // another thread would not, because the system can delay a long timer by several milliseconds.
  if (auto const remaining = deadline - now(); remaining > std::chrono::nanoseconds{0}) {
    std::this_thread::sleep_for(remaining);
  }
}

auto platform::event_clock_now() -> basalt::event_time { return now(); }

auto platform::wake() -> void { detail::push_wake_event(); }

auto platform::poll_events() -> std::vector<platform_event> {
  auto result = std::vector<platform_event>{};
  SDL_Event event{};
  while (SDL_PollEvent(&event)) {
    auto const timestamp = event.common.timestamp == 0 ? now() : event_time(event.common.timestamp);
    switch (event.type) {
    case SDL_EVENT_QUIT:
      append_window_event(result, 0, basalt::window_action::close_requested, timestamp);
      break;
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
      append_window_event(result, event.window.windowID, basalt::window_action::resized, timestamp);
      break;
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
      append_window_event(result, event.window.windowID, basalt::window_action::scale_changed,
                          timestamp);
      break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
      append_window_event(result, event.window.windowID, basalt::window_action::focus_gained,
                          timestamp);
      break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      append_window_event(result, event.window.windowID, basalt::window_action::focus_lost,
                          timestamp);
      break;
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
      append_window_event(result, event.window.windowID, basalt::window_action::close_requested,
                          timestamp);
      break;
    case SDL_EVENT_WINDOW_SHOWN:
    case SDL_EVENT_WINDOW_EXPOSED:
      append_window_event(result, event.window.windowID, basalt::window_action::shown, timestamp);
      break;
    case SDL_EVENT_WINDOW_HIDDEN:
      append_window_event(result, event.window.windowID, basalt::window_action::hidden, timestamp);
      break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
      auto value = basalt::key_event{};
      value.code = translate_key(event.key.key);
      value.pressed = event.key.down;
      value.repeat = event.key.repeat;
      value.modifiers = modifiers(event.key.mod);
      value.time = timestamp;
      result.push_back({event.key.windowID, value});
      break;
    }
    case SDL_EVENT_TEXT_INPUT:
      result.push_back(
          {event.text.windowID,
           basalt::text_input_event{event.text.text != nullptr ? event.text.text : "", timestamp}});
      break;
    case SDL_EVENT_TEXT_EDITING: {
      auto const text = std::string_view{event.edit.text != nullptr ? event.edit.text : ""};
      auto value = basalt::ime_event{};
      value.text = text;
      value.selection_start =
          event.edit.start < 0 ? 0 : utf8_offset(text, static_cast<std::size_t>(event.edit.start));
      value.selection_length =
          event.edit.length < 0
              ? 0
              : utf8_offset(text,
                            static_cast<std::size_t>(event.edit.start < 0 ? 0 : event.edit.start) +
                                static_cast<std::size_t>(event.edit.length)) -
                    value.selection_start;
      value.time = timestamp;
      result.push_back({event.edit.windowID, value});
      break;
    }
    case SDL_EVENT_MOUSE_MOTION: {
      auto value = basalt::pointer_event{};
      value.action = basalt::pointer_action::move;
      value.position = pointer_position(event.motion.x, event.motion.y);
      value.buttons = translate_buttons(event.motion.state);
      value.modifiers = modifiers(SDL_GetModState());
      value.time = timestamp;
      result.push_back({event.motion.windowID, value});
      break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
      auto value = basalt::pointer_event{};
      value.action = event.button.down ? basalt::pointer_action::down : basalt::pointer_action::up;
      value.position = pointer_position(event.button.x, event.button.y);
      value.button = translate_button(event.button.button);
      auto state = static_cast<SDL_MouseButtonFlags>(SDL_GetMouseState(nullptr, nullptr));
      if (value.action == basalt::pointer_action::down) {
        state |= SDL_BUTTON_MASK(event.button.button);
      } else {
        state &= ~SDL_BUTTON_MASK(event.button.button);
      }
      value.buttons = translate_buttons(state);
      value.modifiers = modifiers(SDL_GetModState());
      value.click_count = std::max<std::uint8_t>(1, event.button.clicks);
      value.time = timestamp;
      result.push_back({event.button.windowID, value});
      break;
    }
    case SDL_EVENT_MOUSE_WHEEL: {
      auto value = basalt::wheel_event{};
      auto const direction = event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -1.0f : 1.0f;
      value.position = pointer_position(event.wheel.mouse_x, event.wheel.mouse_y);
      value.delta = {event.wheel.x * direction * wheel_notch_layout_pixels,
                     event.wheel.y * direction * wheel_notch_layout_pixels};
      value.modifiers = modifiers(SDL_GetModState());
      value.precise = true;
      value.time = timestamp;
      result.push_back({event.wheel.windowID, value});
      break;
    }
    case SDL_EVENT_WINDOW_MOUSE_ENTER:
    case SDL_EVENT_WINDOW_MOUSE_LEAVE: {
      auto value = basalt::pointer_event{};
      value.action = event.type == SDL_EVENT_WINDOW_MOUSE_ENTER ? basalt::pointer_action::enter
                                                                : basalt::pointer_action::leave;
      value.time = timestamp;
      result.push_back({event.window.windowID, value});
      break;
    }
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP: {
      auto *const window = detail::window_for(event.tfinger.windowID);
      if (window == nullptr) {
        break;
      }
      auto const extent = detail::viewport(window);
      auto value = basalt::pointer_event{};
      value.action = event.type == SDL_EVENT_FINGER_DOWN ? basalt::pointer_action::down
                     : event.type == SDL_EVENT_FINGER_UP ? basalt::pointer_action::up
                                                         : basalt::pointer_action::move;
      value.position =
          pointer_position(event.tfinger.x * extent.width, event.tfinger.y * extent.height);
      value.kind = basalt::pointer_kind::touch;
      value.modifiers = modifiers(SDL_GetModState());
      value.button = event.type == SDL_EVENT_FINGER_DOWN || event.type == SDL_EVENT_FINGER_UP
                         ? basalt::pointer_button::primary
                         : basalt::pointer_button::none;
      value.buttons = event.type == SDL_EVENT_FINGER_UP ? basalt::pointer_button::none
                                                        : basalt::pointer_button::primary;
      value.time = timestamp;
      result.push_back({event.tfinger.windowID, value});
      break;
    }
    default:
      break;
    }
  }
  auto &callback_errors = *impl_->dialog_callback_errors;
  if (!callback_errors.empty()) {
    auto error = std::exchange(callback_errors.front(), {});
    callback_errors.erase(callback_errors.begin());
    std::rethrow_exception(error);
  }
  return result;
}

auto platform::clipboard_text() const -> std::expected<std::string, platform_error> {
  auto text = std::unique_ptr<char, decltype(&SDL_free)>{SDL_GetClipboardText(), SDL_free};
  if (text == nullptr) {
    return std::unexpected(platform_error{SDL_GetError()});
  }
  return std::string{text.get()};
}

auto platform::set_clipboard_text(std::string_view text) -> std::expected<void, platform_error> {
  auto const value = std::string{text};
  if (!SDL_SetClipboardText(value.c_str())) {
    return std::unexpected(platform_error{SDL_GetError()});
  }
  return {};
}

auto platform::set_cursor(cursor_shape shape) -> std::expected<void, platform_error> {
  auto found = impl_->cursors.find(shape);
  if (found == impl_->cursors.end()) {
    auto system_cursor = SDL_SYSTEM_CURSOR_DEFAULT;
    switch (shape) {
    case cursor_shape::arrow:
      system_cursor = SDL_SYSTEM_CURSOR_DEFAULT;
      break;
    case cursor_shape::text:
      system_cursor = SDL_SYSTEM_CURSOR_TEXT;
      break;
    case cursor_shape::crosshair:
      system_cursor = SDL_SYSTEM_CURSOR_CROSSHAIR;
      break;
    case cursor_shape::pointer:
      system_cursor = SDL_SYSTEM_CURSOR_POINTER;
      break;
    case cursor_shape::resize_horizontal:
      system_cursor = SDL_SYSTEM_CURSOR_EW_RESIZE;
      break;
    case cursor_shape::resize_vertical:
      system_cursor = SDL_SYSTEM_CURSOR_NS_RESIZE;
      break;
    case cursor_shape::wait:
      system_cursor = SDL_SYSTEM_CURSOR_WAIT;
      break;
    }
    auto *const cursor = SDL_CreateSystemCursor(system_cursor);
    if (cursor == nullptr) {
      return std::unexpected(platform_error{SDL_GetError()});
    }
    found = impl_->cursors.emplace(shape, sdl_cursor_handle{cursor}).first;
  }
  if (!SDL_SetCursor(found->second.get())) {
    return std::unexpected(platform_error{SDL_GetError()});
  }
  return {};
}

auto platform::show_open_file_dialog(window_id window, std::span<file_filter const> filters,
                                     file_dialog_callback callback, bool allow_many,
                                     std::string_view default_location) -> void {
  auto state = make_dialog_state(filters, std::move(callback), default_location,
                                 impl_->dialog_callback_errors);
  auto *const parent = detail::window_for(window);
  auto *const state_pointer = state.release();
  SDL_ShowOpenFileDialog(
      on_dialog, state_pointer, parent, state_pointer->filters.data(),
      static_cast<int>(state_pointer->filters.size()),
      state_pointer->default_location.empty() ? nullptr : state_pointer->default_location.c_str(),
      allow_many);
}

auto platform::show_save_file_dialog(window_id window, std::span<file_filter const> filters,
                                     file_dialog_callback callback,
                                     std::string_view default_location) -> void {
  auto state = make_dialog_state(filters, std::move(callback), default_location,
                                 impl_->dialog_callback_errors);
  auto *const parent = detail::window_for(window);
  auto *const state_pointer = state.release();
  SDL_ShowSaveFileDialog(
      on_dialog, state_pointer, parent, state_pointer->filters.data(),
      static_cast<int>(state_pointer->filters.size()),
      state_pointer->default_location.empty() ? nullptr : state_pointer->default_location.c_str());
}

auto platform::start_text_input(window_id window) -> std::expected<void, platform_error> {
  auto *const target = detail::window_for(window);
  if (target == nullptr || !SDL_StartTextInput(target)) {
    return std::unexpected(platform_error{target == nullptr ? "unknown window" : SDL_GetError()});
  }
  return {};
}

auto platform::stop_text_input(window_id window) -> std::expected<void, platform_error> {
  auto *const target = detail::window_for(window);
  if (target == nullptr || !SDL_StopTextInput(target)) {
    return std::unexpected(platform_error{target == nullptr ? "unknown window" : SDL_GetError()});
  }
  return {};
}

auto platform::set_text_input_area(window_id window, basalt::rect area, int cursor_offset)
    -> std::expected<void, platform_error> {
  auto *const target = detail::window_for(window);
  if (target == nullptr) {
    return std::unexpected(platform_error{"unknown window"});
  }
  auto const rectangle =
      SDL_Rect{static_cast<int>(std::floor(area.left())), static_cast<int>(std::floor(area.top())),
               std::max(0, static_cast<int>(std::ceil(area.width()))),
               std::max(0, static_cast<int>(std::ceil(area.height())))};
  if (!SDL_SetTextInputArea(target, &rectangle, cursor_offset)) {
    return std::unexpected(platform_error{SDL_GetError()});
  }
  return {};
}

} // namespace magma
