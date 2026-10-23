# Frame Scheduling

The `frame_scheduler` class decides when the next frame can start. It starts at most one frame for
each display refresh, and it starts a frame for input at once when it can. The header is
`magma/frame_scheduler.h`.

## Why a scheduler

A frame that starts at a bad time costs time or causes a missed refresh. A frame that starts at a
refresh has a full refresh interval to finish. Two frames in one refresh interval waste
work, because the display shows only one of them. A frame for a key press that waits for the next
refresh adds up to one interval of latency.

The scheduler applies these rules in one place. Your event loop asks it what to do.

## Time

The scheduler has no clock. Each function that needs the time takes it as an argument, in
nanoseconds. Use `platform::event_clock_now`, the same clock as the display ticks.

Because the scheduler has no clock, a test can give it any sequence of times. The tests in
`tests/magma/frame_scheduler_test.cpp` do this.

## Requests

These functions tell the scheduler that a frame is wanted:

- `request_frame` asks for a frame. Use it for animations, for frame requests from the widget tree,
  and after a resize.
- `request_input_frame` asks for a frame because of input. It can start sooner than a frame from
  `request_frame`.

Requests combine. Many requests before the next frame cause one frame. `pending` is true while a
request waits.

## Signals from the loop

These functions tell the scheduler what happened:

- `display_refreshed(time, interval)` reports a display tick. After the first call,
  `follows_display` is true. An `interval` of zero or less keeps the current interval.
- `frame_shown` reports that the previous frame is on the screen, or that it drew nothing. Call it
  when `renderer::frame_in_flight` is false.
- `begin_frame(now)` marks the pending frame as started. A request made during the frame causes
  another frame.

## Questions from the loop

These functions tell the loop what to do:

- `frame_due(now)` is true when a frame can start now.
- `wait_timeout(now)` gives the time that the loop can wait. `std::nullopt` means that no frame is
  pending, so the loop can wait with no limit. Zero means that a frame is due now.
- `next_frame_time()` gives the time when the pending frame can start.

## Rules

The scheduler starts the pending frame at the first time that one of these rules allows. The
rules are in order.

1. The first frame starts at once.
2. A frame for input starts at once if the previous frame is on the screen.
3. With display ticks, a frame starts at the first refresh after the previous frame began.
4. With display ticks, a frame starts at once if the previous frame is on the screen and began at
   least one interval ago. Thus the first frame after a pause does not wait for a refresh.
5. With display ticks, if no tick arrives, a frame starts two intervals after the last refresh.
   For example, a hidden window gets no ticks. Frames then continue at the interval.
6. Without display ticks, frames start at least one interval apart.

The default interval is 16,666,667 nanoseconds, which is 60 Hz. Set the real interval in the
constructor or with `set_interval`.

## Displays without ticks

Only macOS 14 and later send display ticks. On other systems, `request_display_tick` returns false.
The scheduler then uses only the interval. Set it from `display_refresh_rate`:

```cpp
auto refresh_interval(magma::platform const &platform, magma::window_id window) -> std::int64_t {
  auto const rate = platform.display_refresh_rate(window).value_or(60.0F);
  return static_cast<std::int64_t>(std::llround(1e9 / rate));
}
```

A window can move to a display with a different rate. When a window event arrives and
`follows_display` is false, set the interval again.

## Input latency

A frame for input waits for the previous frame to reach the screen. If it did not wait, the new
frame would wait inside the GPU driver for the previous one.

On Metal, `renderer::frame_in_flight` is true until the display shows the frame. On Vulkan and
Direct3D 12, it is true until the frame goes to the window system. The raster backend is never in
flight.

With display sync on, Metal shows a frame about two refreshes after it starts. See
[Rendering](rendering.md#metal) for the settings that change this, and
[Benchmarks](benchmarks.md) for the measured latency.
