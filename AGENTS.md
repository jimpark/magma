# AGENTS.md

## Purpose

Magma is a C++23 library that gives Basalt applications their windows, input and frame pacing. It
will also render Basalt paint commands. `README.md` describes the library for its users. This file
holds the rules for changing it.

## Read First

- `spec/CONVENTIONS.md`: mandatory C++ conventions (snake_case, `auto f() -> T`, `#pragma once`,
  `std::expected`, no raw owning pointers, warnings as errors).
- `README.md`: build options and how consumers use the library.

## Layout

| Path | Contents |
| ---- | -------- |
| `include/magma/` | Public headers. `magma/magma.h` includes all of them. |
| `src/magma/platform/` | The SDL 3 platform. `sdl_platform_internal.h` is private. |
| `tests/magma/` | Catch2 tests. The platform tests use SDL's dummy video driver. |
| `tests/consumer/` | A small project that consumes Magma by `find_package`, `add_subdirectory` or `FetchContent`. |
| `cmake/` | Compiler options, dependencies and their pins, the offline source fetcher, install rules, and the include boundary check. |
| `.github/` | The CI and release workflows, and the setup action they share. |

## Rules

- Code lives in namespace `magma`. Quoted includes with a directory must start with `magma/` or
  `basalt/`. The `include_boundaries` test enforces this and checks that `magma/magma.h` includes
  every public header.
- SDL is a private dependency. No public header may include it, and the `include_boundaries` test
  checks this. Graphics APIs and renderer libraries follow the same rule.
- Basalt is a public dependency. Magma uses Basalt's types in its API and does not wrap them.
- Compiler flags go on `magma_compiler_options`, which is linked privately, so they never reach
  consumers.
- New dependencies are pinned in `cmake/dependency_pins.cmake` by release archive and SHA-256, and
  added to `cmake/fetch_sources.cmake`.
- GitHub Actions are pinned by commit hash, with the version in a trailing comment.
- CI builds with GCC, Clang, Apple Clang and MSVC with warnings as errors. Code must compile cleanly
  on all four.
- Tests must run without a display. Use SDL's dummy video driver for platform tests.

## Commands

```sh
just build              # configure and build the debug preset
just test               # build and run all tests
just test sanitize      # the same, with sanitizers
just consumer           # install a release build and build tests/consumer against it
just sources            # download the pinned dependency sources for offline builds
just format             # clang-format every source
```

## Tests

A test that cannot fail is worse than no test. Before trusting a new test, make it fail on purpose
by breaking the code or the expected value, confirm the failure message, and undo the change.

## Documentation

Write all documentation in ASD-STE100 style. The audience is an experienced C++ programmer new to
this library.
