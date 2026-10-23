# Rendering

The `renderer` class draws Basalt paint commands and shows the result in a window. It keeps the
last frame, so each frame draws only the damaged areas. It has one backend for the CPU and three
for the GPU. The header is `magma/renderer.h`.

## Create a renderer

`renderer::create(platform, window, preference)` makes a renderer for one window. It reads the
window's size and display scale as the first metrics. The platform must outlive the renderer.

`renderer::create_offscreen(preference)` makes a renderer that draws into memory only. Use it for
tests and tools. Call `set_metrics` before the first frame. The default preference for an offscreen
renderer is `raster_only`, which gives the reference image.

The preference selects the backends that the renderer can use:

| Preference | Effect |
| ---------- | ------ |
| `automatic` | Use a GPU backend when one can start. Otherwise use `raster`. |
| `raster_only` | Never create a GPU device. |

`active_backend` tells which backend the renderer uses.

Create the window with `preferred_graphics_api(preference)`. This function gives the graphics API
that the preference needs on the current system. A window with the wrong graphics API cannot use
the GPU.

## Metrics

`set_metrics(viewport, scale)` sets the viewport in layout pixels and the display scale. The frame
has `viewport * scale` device pixels. Call `set_metrics` at the start of each frame, with the
values from the widget tree.

A change of device size or scale discards the frame and requests a full redraw.

## Draw a frame

`render(commands, damage, clear)` draws one frame and shows it.

1. It clears each damaged area to the `clear` color.
2. It draws the commands, clipped to the damaged areas.
3. It shows the frame in the window.

The `damage` rectangles are in layout pixels. Use the result of `widget_tree::take_damage`. Use the
theme's background color, such as `background_default`, as `clear`.

If the damage is empty and no full redraw is pending, `render` shows nothing. It returns success.

## Full redraw

`needs_full_redraw` is true until the next frame that draws everything. While it is true, `render`
ignores the damage and draws the whole viewport. Then the commands must cover the whole viewport.

```cpp
auto commands = basalt::paint_commands{};
if (renderer.needs_full_redraw()) {
  basalt::paint_tree(tree, commands);
} else {
  auto bounds = damage.front();
  for (auto const &area : damage) {
    bounds = basalt::bounding_union(bounds, area);
  }
  basalt::paint_tree(tree, commands, bounds);
}
```

When the damage is small, paint only the bounding box of the damage. Paint one box, not each
rectangle separately. If you paint each rectangle separately, a parent's background can cover what
an earlier rectangle drew.

Call `request_full_redraw` when the window shows again after it was hidden or covered. The system
can discard the window's pixels at these times.

## Backends

| Backend | Device | Systems |
| ------- | ------ | ------- |
| `raster` | CPU, with Blend2D | All |
| `metal` | GPU, with Metal | macOS |
| `vulkan` | GPU, with Vulkan | Linux and Windows |
| `d3d12` | GPU, with Direct3D 12 | Windows |

The raster backend is the reference. The golden images in the tests come from it. It draws large
areas on up to four threads and small areas on one thread. It shows the frame with
`platform::present_pixels`.

The GPU backends share one design. Each draws every rectangle, rounded rectangle, stroke, line,
glyph, and image as one instanced quad. The shader calculates the coverage of each pixel exactly.
Glyphs come from an atlas that Blend2D fills, at quarter-pixel positions. A clip that is not
aligned to the axes, and a layer with an opacity, draw into an offscreen texture.

Each GPU backend keeps the frame in an image or texture. It redraws the damaged areas and copies
the image into the window.

Before each frame, all backends skip the commands that are outside the damaged areas.

On Windows, `preferred_graphics_api` gives Direct3D 12 when a Direct3D 12 adapter is available. If
not, it gives Vulkan.

## Text

All backends draw text from the glyph outlines that Basalt gives with `font_face::outline`. Thus
variable fonts show their correct weight. The renderer does not need its own font engine.

## Metal

The Metal layer syncs to the display. The system shows a frame about two refreshes after it
starts. Without display sync, key presses reach the screen sooner, but slow scrolling tears.
Thus display sync is on by default.

These environment variables change the Metal backend. Set them before you create the renderer.

| Variable | Effect |
| -------- | ------ |
| `MAGMA_METAL_REDRAW=full` | Draw each frame from nothing into the window. `needs_full_redraw` is then always true, and `read_pixel` and `read_frame` return nothing for a window. The default is `retained`. |
| `MAGMA_METAL_VSYNC=0` | Turn display sync off. |
| `MAGMA_METAL_DRAWABLES` | The number of drawables, `2` or `3`. The default is 3. |

These settings are for measurement. [Benchmarks](benchmarks.md) gives the results of each one.

The Metal backend compiles its shaders when it starts. The build does not need the Metal
toolchain.

## Vulkan and Direct3D 12

The Vulkan and Direct3D 12 backends wait for the display. Vulkan uses first-in first-out
presentation, and Direct3D 12 uses `Present(1, 0)`. Neither can tell when a frame reaches the
screen. They report the time when they gave the frame to the window system.

The Vulkan backend loads the Vulkan library when it starts. The build needs no Vulkan SDK.

A software device is slower than the raster backend. Thus the Vulkan backend refuses a CPU
device, and the Direct3D 12 backend refuses WARP. These environment variables change this
behavior, and turn on validation:

| Variable | Effect |
| -------- | ------ |
| `MAGMA_VULKAN_ALLOW_SOFTWARE=1` | Accept a CPU device, for example Mesa's lavapipe. |
| `MAGMA_D3D12_ADAPTER=warp` | Use the WARP software adapter. |
| `MAGMA_VULKAN_VALIDATION=1` | Load the Khronos validation layer, if it is installed. |
| `MAGMA_D3D12_VALIDATION=1` | Turn on the Direct3D 12 debug layer. |

## Read pixels

`read_pixel(x, y)` gives the color of one device pixel in the last frame. `read_frame` gives a copy
of the whole frame as a `basalt::raster_image_ptr`. Both return nothing before the first frame.

Use these functions with an offscreen renderer in tests. This example draws one rectangle and
checks a pixel:

```cpp
auto renderer = magma::renderer::create_offscreen();
renderer.set_metrics({100, 100}, 1.0F);
auto const commands = basalt::paint_commands{
    basalt::fill_rect_command{{{10, 10}, {20, 20}}, basalt::rgba{240, 88, 11, 255}}};
auto const whole = basalt::rect{{0, 0}, {100, 100}};
if (!renderer.render(commands, {&whole, 1}, basalt::rgba{255, 255, 255, 255})) {
  return EXIT_FAILURE;
}
auto const pixel = renderer.read_pixel(15, 15);
```

## Frame statistics

The renderer can record when each frame reaches the screen.

- `track_presentations(true)` starts the record. It is off by default.
- `last_frame` gives the number of the last frame that `render` showed, from 1.
- `take_presentations` gives the records since the last call. Each `presentation` has a frame
  number and a time on the platform's event clock. The time is empty when a later frame replaced
  the frame before the display showed it.
- `frame_in_flight` is true while the last frame has not reached the screen. Use it to call
  `frame_scheduler::frame_shown`.

The meaning of the time depends on the backend. Metal reports when the display showed the frame.
The other backends report when they gave the frame to the window system.
