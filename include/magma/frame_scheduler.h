#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>

namespace magma {

// Decides when the next frame may start. It holds no clock: callers pass a
// monotonic time in nanoseconds, so the same rules apply to the event loop and
// to tests. Frame requests coalesce, and frames start at least `interval`
// apart.
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

  auto request_frame() noexcept -> void { pending_ = true; }
  [[nodiscard]] auto pending() const noexcept -> bool { return pending_; }

  // The time the pending frame may start, or nothing when no frame is pending.
  [[nodiscard]] auto next_frame_time() const noexcept -> std::optional<nanoseconds> {
    if (!pending_) {
      return std::nullopt;
    }
    return last_start_ ? std::optional{*last_start_ + interval_} : std::optional<nanoseconds>{0};
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
    pending_ = false;
    last_start_ = now;
  }

private:
  nanoseconds interval_;
  std::optional<nanoseconds> last_start_;
  bool pending_ = false;
};

} // namespace magma
