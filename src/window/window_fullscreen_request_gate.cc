#include "window/window_fullscreen_request_gate.h"

namespace mocktail {
namespace window {

bool WindowFullscreenRequestGate::RequestFromAndroidFlags(int flags, int mask) {
  const std::uint32_t requested_mask = static_cast<std::uint32_t>(mask);
  constexpr std::uint32_t kRelevantFlags =
      kAndroidWindowFlagFullscreen | kAndroidWindowFlagForceNotFullscreen;
  if ((requested_mask & kRelevantFlags) == 0) {
    return false;
  }

  std::lock_guard<std::mutex> lock(mutex_);
  const std::uint32_t requested_flags = static_cast<std::uint32_t>(flags);
  android_flags_ =
      (android_flags_ & ~requested_mask) | (requested_flags & requested_mask);

  const bool fullscreen =
      (android_flags_ & kAndroidWindowFlagFullscreen) != 0 &&
      (android_flags_ & kAndroidWindowFlagForceNotFullscreen) == 0;
  pending_state_ = fullscreen ? 2 : 1;
  return true;
}

bool WindowFullscreenRequestGate::Take(bool* fullscreen) {
  if (fullscreen == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (pending_state_ == 0) {
    return false;
  }
  *fullscreen = pending_state_ == 2;
  pending_state_ = 0;
  return true;
}

void WindowFullscreenRequestGate::Reset() {
  std::lock_guard<std::mutex> lock(mutex_);
  pending_state_ = 0;
  android_flags_ = 0;
}

}  // namespace window
}  // namespace mocktail
