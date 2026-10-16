# Magma

Magma is a C++23 library that gives [Basalt](https://github.com/nadiasvertex/basalt) applications
their windows, input and rendering. Basalt turns a widget tree into paint commands. Magma opens the
windows, translates the operating system's input into Basalt events, decides when to start each
frame, and turns the paint commands into pixels.

## Contents

- **Platform** (`magma/platform.h`): windows, input events as `basalt::event`, the clipboard,
  cursors, file dialogs and text input for input methods. A window that uses software rendering
  shows pixels with `present_pixels`.
- **Frame scheduler** (`magma/frame_scheduler.h`): decides when the next frame can start. It has no
  clock of its own. The caller gives it the time, so the same rules apply in an event loop and in
  tests. It starts frames at the display's refreshes, and it starts a frame for input at once when
  the previous frame is on the screen.
- **Renderer** (`magma/renderer.h`): draws Basalt paint commands into a window, or into memory for
  tests and tools. It keeps the last frame and redraws only the damaged areas.

`magma/magma.h` includes every public header.

The platform uses SDL 3. SDL is a private dependency: no public header includes it, and the
application does not call SDL.

The renderer has two backends:

- `raster` draws on the CPU with [Blend2D](https://blend2d.com) and shows the frame with
  `present_pixels`. It is the reference: the golden images come from it.
- `metal` draws on the GPU with Metal, on macOS. It draws each rect, rounded rect, stroke, line,
  glyph and image as one instanced quad, and its shader calculates the coverage of each pixel
  exactly. Glyphs come from an atlas that Blend2D fills, at quarter-pixel positions. A clip that
  is not axis-aligned and a layer with an opacity draw into an offscreen texture.

Blend2D is a private dependency, and so is Metal. Vulkan and Direct3D 12 backends will follow.
Create the window with the graphics API that `preferred_graphics_api` gives, so that the renderer
can use the GPU. With `backend_preference::automatic`, the renderer uses the GPU when it can, and
`raster` when it cannot. `renderer::active_backend` tells which backend it uses.

Both backends draw text from the glyph outlines that Basalt gives (`font_face::outline`), so
variable fonts show their correct weight. The raster backend draws large areas on up to four
threads and small areas on one thread. Before each frame, both backends skip the commands that are
outside the damaged areas.

The Metal backend keeps the frame in a texture, redraws the damaged areas and copies the texture
into the window. To draw each frame from nothing into the window instead, set the environment
variable `MAGMA_METAL_REDRAW=full` before you create the renderer. Then `needs_full_redraw` is
always true, so send all of the paint commands on each frame. This mode is for comparing the two
methods.

The Metal layer syncs to the display, so the system shows a frame about two refreshes after it
starts. Without display sync, keystrokes reach the screen sooner, but slow scrolling tears. Two
more variables change the layer for measurement: `MAGMA_METAL_VSYNC=0` turns display sync off, and
`MAGMA_METAL_DRAWABLES` (`2` or `3`) sets the number of drawables. The default is three drawables. The Metal backend compiles its shaders when it starts, so the build does not need the
Metal toolchain.

The application keeps its own event loop. `examples/event_loop.cpp` is a complete loop. A typical
loop does these steps:

1. While a frame is pending, call `platform::request_display_tick`. On macOS 14 and later the
   platform then sends one tick at the display's next refresh. Elsewhere it returns false, and the
   scheduler uses the interval from `display_refresh_rate`.
2. Call `wait_for_events` with the timeout that `frame_scheduler::wait_timeout` gives. The wait
   ends at a display tick, at an event, or at the timeout, to the nearest nanosecond that the
   system allows.
3. Give each tick from `take_display_tick` to `display_refreshed`. When
   `renderer::frame_in_flight` is false, call `frame_shown`.
4. Call `poll_events` and send the events to the Basalt widget tree. For a frame caused by input,
   call `request_input_frame`. For other frames, such as animations, call `request_frame`.
5. When `frame_due` is true, call `begin_frame`, paint the tree, and give the paint commands and the
   damaged areas to `renderer::render`.

## Requirements

Magma needs CMake 3.25 or later and a C++23 compiler and standard library. CI builds and tests it
with GCC 14 and Clang 20 on Ubuntu 24.04, Apple Clang on macOS 26, and MSVC from Visual Studio 2022
on Windows. CMake downloads Basalt, SDL 3, Blend2D and, for the tests, Catch2 when it configures the
build, unless you supply them.

On Linux, SDL includes support for X11, Wayland and input methods only when their development
packages are installed. On Ubuntu, install these packages before you configure:

```sh
sudo apt-get install libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev \
  libxss-dev libxkbcommon-dev libwayland-dev libegl1-mesa-dev libdbus-1-dev libibus-1.0-dev
```

## Building

The presets build into `.build/<preset>`:

```sh
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

With [just](https://github.com/casey/just) installed, `just test` does the same. `just --list` shows
the other recipes.

## Using Magma in Another Project

Link the target `magma::magma`. It brings `basalt::basalt` with it. There are three ways to add
Magma to a CMake build.

### FetchContent

```cmake
include(FetchContent)
FetchContent_Declare(magma
  GIT_REPOSITORY https://github.com/nadiasvertex/magma.git
  GIT_TAG v0.1.0)
FetchContent_MakeAvailable(magma)

target_link_libraries(my_app PRIVATE magma::magma)
```

### add_subdirectory

```cmake
add_subdirectory(third_party/magma)
target_link_libraries(my_app PRIVATE magma::magma)
```

### find_package

Install Magma, or unpack a [release archive](#release-archives), and add its prefix to
`CMAKE_PREFIX_PATH`:

```sh
cmake --preset release
cmake --build --preset release
cmake --install .build/release --prefix /opt/magma
```

```cmake
find_package(magma 0.1 CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE magma::magma)
```

The install also puts Basalt, Blend2D and the bundled SDL into the prefix, and `find_package(magma)` finds
them there. When Magma is included in another project, its tests and install rules are off by
default, and its warning flags apply only to its own sources. `tests/consumer` is a small project
that uses each of these methods.

### Basalt Version

Magma uses an installed Basalt when `find_package` finds a compatible version. If not, it builds
the Basalt release named in `cmake/dependency_pins.cmake`. If your project declares Basalt with
`FetchContent_Declare` before it adds Magma, Magma uses your Basalt.

## Release Archives

Each release on GitHub has prebuilt archives with the headers, Debug and Release static libraries
and the CMake packages for Magma, Basalt, Blend2D and SDL, plus a `SHA256SUMS` file. The Debug libraries
have a `d` suffix, and `find_package` picks the configuration that matches your build.

| Archive | Built with | Needs |
| ------- | ---------- | ----- |
| `magma-<version>-linux-x86_64.tar.gz` | GCC 14 on Ubuntu 24.04 | glibc 2.39 or later and the libstdc++ from GCC 14 or later |
| `magma-<version>-macos-arm64.tar.gz` | Apple Clang on macOS 26 | macOS 26 or later on Apple silicon |
| `magma-<version>-windows-x64.zip` | MSVC from Visual Studio 2022 | Visual Studio 2022 17.14 or later, dynamic runtime (`/MD`, `/MDd`) |

For other setups, build from source with one of the methods above.

## Options

| Option | Default | Effect |
| ------ | ------- | ------ |
| `MAGMA_BUILD_TESTS` | on when top level | Build the Catch2 test suite. |
| `MAGMA_BUILD_BENCHMARKS` | off | Build the Catch2 benchmarks. The `benchmark` preset sets it. |
| `MAGMA_BUILD_EXAMPLES` | on when top level | Build the programs in `examples/`. |
| `MAGMA_INSTALL` | on when top level | Add install rules and the CMake package. |
| `MAGMA_METAL` | on for Clang on macOS | Build the Metal backend. It is written in Objective-C++, which GCC does not compile. |
| `MAGMA_SDL` | `bundled` | `bundled` builds a static SDL from the pinned release. `system` uses an installed SDL 3.2 or later. |
| `MAGMA_WARNINGS_AS_ERRORS` | on when top level | Treat warnings in Magma's sources as errors. |
| `MAGMA_SANITIZE` | off | Address and undefined-behavior sanitizers on Clang and GCC. |

## Dependencies

The dependency versions and their checksums are in `cmake/dependency_pins.cmake`. To build without
network access later, download and unpack the pinned sources once. Then give the generated initial
cache file when you configure:

```sh
cmake -D MAGMA_SOURCES_DIR=.build/sources -P cmake/fetch_sources.cmake
cmake --preset debug -C .build/sources/init.cmake
```

The script also gets HarfBuzz, which Basalt needs. You can also set
`FETCHCONTENT_SOURCE_DIR_<NAME>` to your own copy of each dependency.

## Tests

The platform tests use SDL's dummy video driver, so they need no display. They push SDL events and
check the Basalt events that `poll_events` makes from them.

The golden-image tests render fixed lists of paint commands and compare each frame with a PNG file
in `tests/data/golden`. Together the lists use every paint command. Each color channel can differ by
3 levels, and 0.2 percent of the pixels can differ by more. When a test fails, it writes the frame
and an image of the differences to `.build/<preset>/tests/golden` and gives their names.

Each golden-image test also runs on the GPU backend, if the computer has one, and compares the GPU
frame with the same image. Scenes with only straight edges and images have the same tolerance as
the CPU. For curves and text, 15 percent of the pixels can differ by up to 36 levels. Blend2D
changes curves into short lines that are a little inside the curve, and the GPU puts glyphs at
quarter-pixel positions, so many edge pixels are different by a small quantity. Pixels inside
shapes must agree. The files for GPU frames have the backend name in their names, for example
`text-metal.png`. GPU frames never replace the golden images.

The GPU scene tests check how paint commands become GPU instances and batches. They do not need a
GPU, so they run on every platform.

To write new golden images, run the tests with `MAGMA_UPDATE_GOLDEN=1`. Examine each new image
before you commit it.

## Benchmarks

`just bench` builds the `benchmark` preset and replays one page of an article: a heading, about
30 lines of text, a code block and some rules, in a window of 1200 by 800 layout pixels. It
measures a full redraw, as when the page scrolls, one line, as when you type, and the caret. It
also measures the first frame of a new renderer, with empty caches.

Each case runs on the CPU and, if the computer has one, on the GPU. A GPU case waits until the GPU
has finished the frame and reads back one pixel. For small areas, this round trip is most of the
time, so these numbers do not show how a window performs. To compare the backends in a window, use
the frame statistics of the application.

## License

Magma is under the Apache License 2.0; see `LICENSE`. SDL and Blend2D are under the zlib license,
and Basalt is under the Apache License 2.0. Installs and release archives include their licenses.
The test font, Noto Sans, is under the SIL Open Font License; see `tests/data/fonts/OFL.txt`.
