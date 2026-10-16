#pragma once

#include "magma/platform.h"

#include <QuartzCore/CABase.h>

#include <chrono>
#include <cstdint>

namespace magma::detail {

// Converts a Core Animation time (`CACurrentMediaTime`, in seconds) to the platform's event
// clock. Both clocks count the same ticks, so only their origins differ.
[[nodiscard]] inline auto media_time_to_event_clock(double seconds) -> std::chrono::nanoseconds {
  auto const offset =
      platform::event_clock_now().count() - static_cast<std::int64_t>(CACurrentMediaTime() * 1e9);
  return std::chrono::nanoseconds{static_cast<std::int64_t>(seconds * 1e9) + offset};
}

} // namespace magma::detail
