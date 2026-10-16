# Magma

Magma is a C++23 library that gives [Basalt](https://github.com/nadiasvertex/basalt) applications
their windows and input. Basalt turns a widget tree into paint commands. Magma opens the windows,
translates the operating system's input into Basalt events, and decides when to start each frame.
Magma will also turn paint commands into pixels. That renderer is not in the library yet.

## Contents

- **Platform** (`magma/platform.h`): windows, input events as `basalt::event`, the clipboard,
  cursors, file dialogs and text input for input methods. A window that uses software rendering
  shows pixels with `present_pixels`.
- **Frame scheduler** (`magma/frame_scheduler.h`): decides when the next frame can start. It has no
  clock of its own. The caller gives it the time, so the same rules apply in an event loop and in
  tests.

`magma/magma.h` includes every public header.

The platform uses SDL 3. SDL is a private dependency: no public header includes it, and the
application does not call SDL.

The application keeps its own event loop. A typical loop does these steps:

1. Call `wait_for_events` with the timeout that `frame_scheduler::wait_timeout` gives.
2. Call `poll_events` and send the events to the Basalt widget tree.
3. If the tree needs a new frame, call `request_frame`.
4. When `frame_due` is true, call `begin_frame`, then paint and present the frame.

## Requirements

Magma needs CMake 3.25 or later and a C++23 compiler and standard library. CI builds and tests it
with GCC 14 and Clang 20 on Ubuntu 24.04, Apple Clang on macOS 26, and MSVC from Visual Studio 2022
on Windows. CMake downloads Basalt, SDL 3 and, for the tests, Catch2 when it configures the build,
unless you supply them.

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

The install also puts Basalt and the bundled SDL into the prefix, and `find_package(magma)` finds
them there. When Magma is included in another project, its tests and install rules are off by
default, and its warning flags apply only to its own sources. `tests/consumer` is a small project
that uses each of these methods.

### Basalt Version

Magma uses an installed Basalt when `find_package` finds a compatible version. If not, it builds
the Basalt release named in `cmake/dependency_pins.cmake`. If your project declares Basalt with
`FetchContent_Declare` before it adds Magma, Magma uses your Basalt.

## Release Archives

Each release on GitHub has prebuilt archives with the headers, Debug and Release static libraries
and the CMake packages for Magma, Basalt and SDL, plus a `SHA256SUMS` file. The Debug libraries
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
| `MAGMA_INSTALL` | on when top level | Add install rules and the CMake package. |
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

## License

Magma is under the Apache License 2.0; see `LICENSE`. SDL is under the zlib license, and Basalt is
under the Apache License 2.0. Installs and release archives include their licenses.
