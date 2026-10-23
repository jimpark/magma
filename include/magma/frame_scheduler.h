#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

namespace magma {

// Decides when the next frame may start. It holds no clock: callers pass a
// monotonic time in nanoseconds, so the same rules apply to the event loop and
// to tests. Frame requests coalesce.
//
// Until the first `display_refreshed`, frames start `interval` apart. The interval counts from
// the time a frame was due, not the time it began, so an event loop that wakes a little late does
// not slow every later frame by that delay. A frame that begins an interval or more after it was
// due counts from the time it began.
// After it, a frame starts at the first display refresh after the previous
// frame began, so each refresh gets at most one frame. When the previous frame
// is shown and began at least `interval` ago, the frame starts at once, so the
// first frame after a pause does not wait for a refresh. If no refresh
// arrives, for example because the window is hidden, frames continue at
// `interval`.
//
// A frame requested for input starts at once when the previous frame has been
// shown, so that a keystroke reaches the screen without waiting for a refresh.
class frame_scheduler {
public:
  using nanoseconds = std::int64_t;

  static constexpr nanoseconds default_interval = 16'666'667;

  explicit frame_scheduler(nanoseconds interval = default_interval) noexcept
      : interval_(std::max<nanoseconds>(interval, 0)) {}

  auto set_interval(nanoseconds interval) noexcept -> void {
    interval_ = std::max<nanoseconds>(interval, 0);
  }
  [[nodiscard]] auto interval() const noexcept -> nanoseconds { return interval_; }

  // Tells the scheduler that the display refreshed at `time` and refreshes
  // every `interval`. A zero or negative interval keeps the current one.
  auto display_refreshed(nanoseconds time, nanoseconds interval) noexcept -> void {
    last_refresh_ = time;
    if (interval > 0) {
      interval_ = interval;
    }
  }
  [[nodiscard]] auto follows_display() const noexcept -> bool { return last_refresh_.has_value(); }

  auto request_frame() noexcept -> void { pending_ = true; }
  // Requests a frame because of input.
  auto request_input_frame() noexcept -> void {
    pending_ = true;
    input_ = true;
  }
  [[nodiscard]] auto pending() const noexcept -> bool { return pending_; }

  // Tells the scheduler that the frame that began last is on the screen, or
  // that it drew nothing.
  auto frame_shown() noexcept -> void { shown_ = true; }

  // The time the pending frame may start, or nothing when no frame is pending.
  // While the scheduler waits for a display refresh, this is the time it stops
  // waiting for one.
  [[nodiscard]] auto next_frame_time() const noexcept -> std::optional<nanoseconds> {
    if (!pending_) {
      return std::nullopt;
    }
    if (!last_start_) {
      return nanoseconds{0};
    }
    if (input_ && shown_) {
      return *last_start_;
    }
    if (last_refresh_) {
      if (*last_refresh_ > *last_start_) {
        return *last_refresh_;
      }
      if (shown_) {
        return *last_start_ + interval_;
      }
      return std::max(*last_refresh_ + 2 * interval_, *last_start_ + interval_);
    }
    return *last_start_ + interval_;
  }

  [[nodiscard]] auto frame_due(nanoseconds now) const noexcept -> bool {
    auto const next = next_frame_time();
    return next && now >= *next;
  }

  // How long an event loop may block: nothing means indefinitely, zero means a
  // frame is due now.
  [[nodiscard]] auto wait_timeout(nanoseconds now) const noexcept -> std::optional<nanoseconds> {
    auto const next = next_frame_time();
    if (!next) {
      return std::nullopt;
    }
    return std::max<nanoseconds>(*next - now, 0);
  }

  // Marks the pending frame as started. A request made while the frame is being
  // produced schedules another frame.
  auto begin_frame(nanoseconds now) noexcept -> void {
    last_start_ = start_time(now);
    pending_ = false;
    input_ = false;
    shown_ = false;
  }

private:
  // The time a frame that begins at `now` counts as started.
  [[nodiscard]] auto start_time(nanoseconds now) const noexcept -> nanoseconds {
    if (!pending_ || !last_start_ || last_refresh_ || (input_ && shown_)) {
      return now;
    }
    auto const due = *last_start_ + interval_;
    return now >= due && now - due < interval_ ? due : now;
  }

  nanoseconds interval_;
  std::optional<nanoseconds> last_start_;
  std::optional<nanoseconds> last_refresh_;
  bool pending_ = false;
  bool input_ = false;
  bool shown_ = false;
};

} // namespace magma
