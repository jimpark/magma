# Benchmarks

This file records how fast Magma draws in a real application, and the decisions that the numbers
support. The numbers come from med, the MEPSA editor, which can draw with its own Skia renderer or
with Magma's renderer. The latest results show that Magma's Metal backend is as fast as Skia or
faster on every measurement. Display pacing removed most missed frames while scrolling. Keystrokes
reach the screen within one refresh only when display sync is off, but then slow scrolling
tears, so display sync stays on by default.

## Method

- Computer: MacBook Pro, M2 Max. The window was on a display that reports 60 Hz.
- Program: med in a release build, window size 1180x780 at scale 2, with the article `test2.mepsa`.
- Command: `just bench-frames 3 test2.mepsa --renderer <skia|magma>` in med. Environment variables
  select Magma's Metal settings. Each value in the tables is the median of three runs.
- Scripts: `scroll` scrolls at a constant rate, `fling` sends trackpad flings, and `type` types a
  paragraph at 80 ms for each key.
- A missed refresh is a gap between two presented frames that is longer than one refresh interval,
  while the script runs. Gaps longer than four intervals are pauses in the script and do not count.
- Key latency is the time from the key event to the time the display showed the frame with the
  key. With display sync on, Metal reports the time the display showed the frame. The CPU backends
  report the time they gave the frame to the window system, so their latency is too low by up to
  one refresh, and their missed refreshes are not visible.

## Results

Magma at version 0.3.0, with display pacing. Times are in milliseconds. The rows that say
"display sync off" set `MAGMA_METAL_VSYNC=0`.

| Configuration | Startup | Scroll misses | Fling misses | Scroll draw p50 / p99 | Key latency p50 / p99 | Keys within one refresh |
| --- | --- | --- | --- | --- | --- | --- |
| Skia on Metal | 351 | 3 | 3 | 0.84 / 4.48 | 32.0 / 42.0 | 0% |
| Magma Metal, retained frame | 305 | 1 | 1 | 0.62 / 13.29 | 31.4 / 41.4 | 0% |
| Magma Metal, full redraw | 300 | 4 | 2 | 0.48 / 1.65 | 32.3 / 39.0 | 0% |
| Magma Metal, display sync off | 332 | 2 | 3 | 0.52 / 1.89 | 8.4 / 21.1 | 97% |
| Magma Metal, full redraw, display sync off | 322 | 3 | 3 | 0.42 / 1.72 | 9.2 / 12.6 | 99% |
| Skia on the CPU | 284 | 0 | 0 | 8.31 / 9.42 | 18.5 / 22.3 | 28% |
| Magma on the CPU | 270 | 0 | 0 | 3.87 / 9.63 | 14.2 / 19.3 | 87% |

The baseline before Magma's display pacing was Skia on Metal with a 60 Hz timer. It missed 11 or
12 refreshes in each scroll run, and its median key latency was about 31 ms.

## Decisions

- Damage strategy: the retained frame stays the default. It missed fewer refreshes while scrolling
  than the full redraw, and its key latency is the same. `MAGMA_METAL_REDRAW=full` stays available
  until the external display is measured.
- Display pacing: frames follow the display link. Scroll misses fell from about 11 to 1 for Magma
  and to 3 for Skia.
- Display sync: with sync on, a frame reaches the screen about two refreshes after it starts. This
  is the system compositor, not the renderer. Only sync off meets the typing target, but with it
  slow scrolling in med tears in a window, in waves across the text. Sync stays on by default.
  `MAGMA_METAL_VSYNC=0` turns it off.
- Drawable count: two drawables gave no clear change in one run, so the default of three stays.

## Open Measurements

- The 120 Hz built-in display and the external display, each with the window on it.
- A way to meet the typing target with display sync on, for example by starting the frame for a
  keystroke later in the refresh, or by presenting it at a set time.
- Key latency includes med's own work for each key, which grows from about 0.1 ms to about 4 ms
  as the typed paragraph gets longer.
