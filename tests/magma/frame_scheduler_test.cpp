#include <catch2/catch_test_macros.hpp>

#include "magma/frame_scheduler.h"

using magma::frame_scheduler;

TEST_CASE("frame scheduler is idle until a frame is requested", "[scheduler]") {
  auto scheduler = frame_scheduler{100};
  CHECK_FALSE(scheduler.pending());
  CHECK_FALSE(scheduler.next_frame_time().has_value());
  CHECK_FALSE(scheduler.wait_timeout(0).has_value());
  CHECK_FALSE(scheduler.frame_due(1'000));
}

TEST_CASE("frame scheduler starts the first frame immediately", "[scheduler]") {
  auto scheduler = frame_scheduler{100};
  scheduler.request_frame();
  CHECK(scheduler.frame_due(0));
  CHECK(scheduler.wait_timeout(50) == 0);
}

TEST_CASE("frame scheduler spaces frames by the interval", "[scheduler]") {
  auto scheduler = frame_scheduler{100};
  scheduler.request_frame();
  scheduler.begin_frame(1'000);
  CHECK_FALSE(scheduler.pending());

  scheduler.request_frame();
  CHECK_FALSE(scheduler.frame_due(1'050));
  CHECK(scheduler.wait_timeout(1'040) == 60);
  CHECK(scheduler.frame_due(1'100));
  CHECK(scheduler.wait_timeout(1'500) == 0);
}

TEST_CASE("frame scheduler coalesces repeated requests", "[scheduler]") {
  auto scheduler = frame_scheduler{100};
  scheduler.request_frame();
  scheduler.request_frame();
  scheduler.begin_frame(10);
  CHECK_FALSE(scheduler.pending());
}

TEST_CASE("frame scheduler keeps a request made during a frame", "[scheduler]") {
  auto scheduler = frame_scheduler{100};
  scheduler.request_frame();
  scheduler.begin_frame(0);
  scheduler.request_frame();
  CHECK(scheduler.next_frame_time() == 100);
}

TEST_CASE("frame scheduler clamps a negative interval", "[scheduler]") {
  auto scheduler = frame_scheduler{-5};
  CHECK(scheduler.interval() == 0);
  scheduler.set_interval(-1);
  CHECK(scheduler.interval() == 0);
}
