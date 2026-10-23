# Platform

The `platform` class connects your application to the operating system. It creates windows,
translates input into Basalt events, and supplies the clipboard, cursors, file dialogs, and text
input. It also sends display ticks for frame pacing. The header is `magma/platform.h`.

## Create the platform

Call `platform::create` once, at the start of the program, on the main thread. It returns a
`platform` or a `platform_error`.

```cpp
auto created = magma::platform::create();
if (!created) {
  std::cerr << created.error().message << '\n';
  return EXIT_FAILURE;
}
auto platform = std::move(*created);
```

The platform can move but cannot copy. Keep it alive while windows and renderers exist.

## Windows

`create_window` takes a `window_config` and returns a `window_id`.

| Field | Default | Meaning |
| ----- | ------- | ------- |
| `graphics` | `software` | The drawing API of the window. It cannot change after the window exists. |
| `title` | empty | The title in the title bar. |
| `size` | 960 by 640 | The size in layout pixels. |
| `resizable` | `true` | The user can change the size. |
| `high_pixel_density` | `true` | The window uses the full resolution of a high-density display. |

Set `graphics` to the value from `preferred_graphics_api`. If you set `software`, the renderer
can use only the raster backend.

These functions read and change a window:

- `window_size` gives the size in layout pixels. `pixel_size` gives the size in device pixels.
- `display_scale` gives the number of device pixels for each layout pixel.
- `display_refresh_rate` gives the refresh rate in hertz, if the system reports one.
- `window_position` and `set_window_position` use layout pixels on the desktop.
- `set_window_size`, `set_window_title`, and `set_fullscreen` change the window.
  `set_fullscreen(window, false)` returns the window to its earlier size and position.
- `system_dark_mode` is true when the operating system uses a dark appearance.
- `destroy_window` closes the window.

Functions that read a window return `std::nullopt` for an unknown window ID.

## Events

`poll_events` returns all queued events as a `std::vector<platform_event>`. Each item holds the
window ID and a `basalt::event`. Give the event to the `event_router` of that window's tree.

The platform makes these Basalt events:

| Basalt event | Source |
| ------------ | ------ |
| `window_event` | Resize, scale change, focus change, close request, shown, and hidden |
| `key_event` | Key down and key up, with repeat and modifiers |
| `text_input_event` | Text that the keyboard or an input method commits |
| `ime_event` | The composition text of an input method, with its selection |
| `pointer_event` | Mouse movement, buttons, enter, and leave. Touch also makes pointer events. |
| `wheel_event` | Mouse wheel and trackpad scroll, in layout pixels |

A `resized` window event holds the new viewport. A `scale_changed` event holds the new display
scale. Basalt's `event_router` uses these values to update the tree. A `close_requested` event
with window 0 is a quit request for the whole application.

Event times are on the platform's event clock. `platform::event_clock_now` gives the current time
on that clock. Use it for the frame scheduler too.

## Wait for events

`wait_for_events` blocks until one of these occurs:

- An event is queued.
- A display tick arrives.
- Another thread calls `wake`.
- The timeout passes. A timeout of `std::nullopt` waits with no limit.

The events stay in the queue. Call `poll_events` after the wait to get them.

`wake` is the only function that you can call from another thread. Call it when a background task
finishes, so that the main loop can read the result.

## Display ticks

A display tick tells when the display showed its latest frame, and the time until its next
refresh. The frame scheduler uses ticks to start frames at the display's refreshes.

`request_display_tick` asks for one tick at the next refresh of the window's display. The tick
ends a wait for events. `take_display_tick` returns it. Ask again for each tick. If you do not ask,
no ticks arrive, so an idle application uses no processor time.

Only macOS 14 and later can send ticks. On other systems, `request_display_tick` returns false.
Then use `display_refresh_rate` to set the scheduler's interval. See
[Frame Scheduling](frame-scheduling.md).

## Live resize

On some systems, the operating system holds the event loop while the user drags the edge of a
window. On macOS, `poll_events` does not run during the drag. Without a frame, the system stretches
the last frame until the drag ends.

`set_live_resize_callback` sets a function that runs for each resize during the drag. It runs on
the main thread, inside the system's resize loop. Give the event to the tree and draw one frame,
then return. The same events also come from the next `poll_events`. An empty function removes the
callback.

```cpp
platform.set_live_resize_callback([&](magma::platform_event const &resize) {
  router.dispatch(resize.event);
  run_frame();
});
```

Do not let the callback run while your loop is already in `poll_events` or in a frame. Use a flag
to prevent this. Remove the callback before the objects that it uses are destroyed.

## Clipboard and cursors

`clipboard_text` returns the text on the clipboard. `set_clipboard_text` replaces it. Both use
UTF-8.

`set_cursor` changes the pointer shape for the application. The shapes are `arrow`, `text`,
`crosshair`, `pointer`, `resize_horizontal`, `resize_vertical`, and `wait`.

## File dialogs

`show_open_file_dialog` and `show_save_file_dialog` show the system's file dialog. They return at
once. The callback runs when the user closes the dialog.

- The callback receives `std::nullopt` when the user cancels the dialog.
- Otherwise it receives the selected paths.
- Each `file_filter` has a name, such as `"Images"`, and a pattern, such as `"png;jpg"`.
- `allow_many` lets the user select more than one file in the open dialog.
- `default_location` is the first folder or file name that the dialog shows.

If the callback throws an exception, the platform keeps it. The next call to `poll_events` throws
it again.

Basalt widgets that need a file use the tree's `file_dialog`. To connect them, set a function with
`widget_tree::set_file_dialog` that calls these platform functions.

## Text input

Text fields need text input from the keyboard and from input methods. Call `start_text_input` for
the window to receive `text_input_event` and `ime_event` values. Call `stop_text_input` to stop.

`set_text_input_area` tells the input method where the text field is, in layout pixels. The input
method puts its candidate window near this area. `cursor_offset` is the caret's horizontal offset
inside the area.

## Surfaces for other renderers

The platform also gives the window's surface to a renderer outside Magma. These functions exist
for applications that still use their own renderer. They will become private when Magma's
renderer replaces those renderers.

| Function | Use |
| -------- | --- |
| `present_pixels` | Copies premultiplied BGRA pixels to a `software` window and shows the damaged areas. |
| `metal_layer` | Gives the `CAMetalLayer` of a `metal` window. |
| `vulkan_instance_extensions`, `create_vulkan_surface`, `destroy_vulkan_surface` | Make a Vulkan surface for a `vulkan` window. |
| `win32_window_handle` | Gives the `HWND` of a window on Windows. |
| `create_gl_context`, `gl_proc_address`, `swap_gl_buffers` | Use OpenGL with an `opengl` window. |

Do not use these functions with Magma's `renderer`. The renderer uses the surface itself.
