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

TEST_CASE("frame scheduler does not add a late start to the next interval", "[scheduler]") {
  auto scheduler = frame_scheduler{100};
  scheduler.request_frame();
  scheduler.begin_frame(1'000);
  for (auto const due : {1'100, 1'200, 1'300}) {
    scheduler.request_frame();
    CHECK(scheduler.next_frame_time() == due);
    // The event loop wakes 3 ns after the frame was due.
    scheduler.begin_frame(due + 3);
  }
  scheduler.request_frame();
  CHECK(scheduler.next_frame_time() == 1'400);
}

TEST_CASE("frame scheduler counts from the start of a frame that began an interval late",
          "[scheduler]") {
  auto scheduler = frame_scheduler{100};
  scheduler.request_frame();
  scheduler.begin_frame(1'000);
  scheduler.request_frame();
  scheduler.begin_frame(1'250);
  scheduler.request_frame();
  CHECK(scheduler.next_frame_time() == 1'350);
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

TEST_CASE("frame scheduler starts frames at display refreshes once it has one", "[scheduler]") {
  auto scheduler = frame_scheduler{100};
  scheduler.display_refreshed(1'000, 50);
  CHECK(scheduler.follows_display());
  CHECK(scheduler.interval() == 50);
  scheduler.request_frame();
  scheduler.begin_frame(1'002);

  scheduler.request_frame();
  CHECK_FALSE(scheduler.frame_due(1'049));
  scheduler.display_refreshed(1'050, 50);
  CHECK(scheduler.frame_due(1'051));
  CHECK(scheduler.wait_timeout(1'051) == 0);
}

TEST_CASE("frame scheduler ignores a refresh from before the frame began", "[scheduler]") {
  auto scheduler = frame_scheduler{100};
  scheduler.display_refreshed(1'000, 50);
  scheduler.request_frame();
  scheduler.begin_frame(1'010);
  scheduler.display_refreshed(1'000, 50);
  scheduler.request_frame();
  CHECK_FALSE(scheduler.frame_due(1'020));
}

TEST_CASE("frame scheduler keeps drawing at the interval when refreshes stop", "[scheduler]") {
  auto scheduler = frame_scheduler{100};
  scheduler.display_refreshed(1'000, 50);
  scheduler.request_frame();
  scheduler.begin_frame(1'002);
  scheduler.request_frame();
  // One refresh may be missed; the frame starts after two.
  CHECK(scheduler.next_frame_time() == 1'100);

  scheduler.begin_frame(1'100);
  scheduler.request_frame();
  // A refresh long ago does not make every frame due at once.
  CHECK(scheduler.next_frame_time() == 1'150);
}

TEST_CASE("frame scheduler starts an input frame at once when the last frame is shown",
          "[scheduler]") {
  auto scheduler = frame_scheduler{100};
  scheduler.display_refreshed(1'000, 50);
  scheduler.request_frame();
  scheduler.begin_frame(1'002);

  scheduler.request_input_frame();
  CHECK_FALSE(scheduler.frame_due(1'010));
  scheduler.frame_shown();
  CHECK(scheduler.frame_due(1'010));

  scheduler.begin_frame(1'010);
  scheduler.request_frame();
  scheduler.frame_shown();
  CHECK_FALSE(scheduler.frame_due(1'020));
}

TEST_CASE("frame scheduler holds an input frame in timer mode until the last frame is shown",
          "[scheduler]") {
  auto scheduler = frame_scheduler{100};
  scheduler.request_frame();
  scheduler.begin_frame(0);
  scheduler.request_input_frame();
  CHECK(scheduler.next_frame_time() == 100);
  scheduler.frame_shown();
  CHECK(scheduler.next_frame_time() == 0);
}

TEST_CASE("frame scheduler starts the first frame after a pause without waiting for a refresh",
          "[scheduler]") {
  auto scheduler = frame_scheduler{100};
  scheduler.display_refreshed(1'000, 50);
  scheduler.request_frame();
  scheduler.begin_frame(1'002);
  scheduler.frame_shown();

  scheduler.request_frame();
  CHECK_FALSE(scheduler.frame_due(1'030));
  CHECK(scheduler.next_frame_time() == 1'052);
  CHECK(scheduler.frame_due(1'500));
}
