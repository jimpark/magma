#include "magma/platform/media_clock.h"

#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>

#include <chrono>
#include <utility>

#include "display_link.h"

namespace magma::detail {
namespace {
class cocoa_display_link;
} // namespace
} // namespace magma::detail

// Receives the display link's callbacks. The link keeps it alive until it is invalidated.
@interface MagmaDisplayLinkTarget : NSObject
@property(nonatomic, assign) magma::detail::cocoa_display_link *owner;
- (void)tick:(id)link;
@end

namespace magma::detail {
namespace {

// A CADisplayLink made by the window, so it follows the window from one display to another. It
// runs on the main run loop, which SDL runs while it waits for events.
class cocoa_display_link final : public display_link {
public:
  cocoa_display_link(NSWindow *window, std::function<void()> wake) : wake_(std::move(wake)) {
    target_ = [[MagmaDisplayLinkTarget alloc] init];
    target_.owner = this;
    if (@available(macOS 14.0, *)) {
      CADisplayLink *const link = [window displayLinkWithTarget:target_ selector:@selector(tick:)];
      link.paused = YES;
      [link addToRunLoop:[NSRunLoop mainRunLoop] forMode:NSRunLoopCommonModes];
      link_ = link;
    }
  }

  cocoa_display_link(cocoa_display_link const &) = delete;
  auto operator=(cocoa_display_link const &) -> cocoa_display_link & = delete;

  ~cocoa_display_link() override {
    target_.owner = nullptr;
    if (@available(macOS 14.0, *)) {
      [static_cast<CADisplayLink *>(link_) invalidate];
    }
  }

  [[nodiscard]] auto valid() const -> bool { return link_ != nil; }

  auto request() -> void override {
    if (@available(macOS 14.0, *)) {
      static_cast<CADisplayLink *>(link_).paused = NO;
    }
  }

  [[nodiscard]] auto take() -> std::optional<display_tick> override {
    return std::exchange(latest_, std::nullopt);
  }

  auto tick(id link) -> void {
    if (@available(macOS 14.0, *)) {
      CADisplayLink *const display = link;
      display.paused = YES;
      latest_ = display_tick{
          .time = media_time_to_event_clock(display.timestamp),
          .interval = std::chrono::nanoseconds{static_cast<std::int64_t>(
              (display.targetTimestamp - display.timestamp) * 1e9)},
      };
      wake_();
    }
  }

private:
  std::function<void()> wake_;
  MagmaDisplayLinkTarget *target_ = nil;
  // A CADisplayLink, kept as `id` because the class exists only on macOS 14 and later.
  id link_ = nil;
  std::optional<display_tick> latest_;
};

} // namespace

auto make_display_link(SDL_Window *window, std::function<void()> wake)
    -> std::unique_ptr<display_link> {
  NSWindow *const cocoa = (__bridge NSWindow *)SDL_GetPointerProperty(
      SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
  if (cocoa == nil) {
    return nullptr;
  }
  auto link = std::make_unique<cocoa_display_link>(cocoa, std::move(wake));
  if (!link->valid()) {
    return nullptr;
  }
  return link;
}

} // namespace magma::detail

@implementation MagmaDisplayLinkTarget
- (void)tick:(id)link {
  if (self.owner != nullptr) {
    self.owner->tick(link);
  }
}
@end
