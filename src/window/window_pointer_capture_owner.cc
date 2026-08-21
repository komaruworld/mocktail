#include "window/window_pointer_capture_owner.h"

#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_video.h>

#include <cstdio>

namespace mocktail {
namespace window {

WindowPointerCaptureOwner::WindowPointerCaptureOwner(
    PointerCaptureBackend* backend)
    : backend_(backend) {}

WindowPointerCaptureOwner::~WindowPointerCaptureOwner() { Shutdown(); }

bool WindowPointerCaptureOwner::RegisterQuery(MouseLockQueryCallback callback,
                                              void* context) {
  if (callback == nullptr || context == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (clearing_ || callback_ != nullptr) {
    return false;
  }
  callback_ = callback;
  context_ = context;
  return true;
}

void WindowPointerCaptureOwner::ClearQuery() {
  std::unique_lock<std::mutex> lock(mutex_);
  condition_.wait(lock, [this] { return !clearing_; });
  clearing_ = true;
  callback_ = nullptr;
  context_ = nullptr;
  condition_.wait(lock, [this] { return in_flight_ == 0; });
  clearing_ = false;
  lock.unlock();
  condition_.notify_all();
}

bool WindowPointerCaptureOwner::Pump(bool text_input_active) {
  MouseLockQueryCallback callback = nullptr;
  void* context = nullptr;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!clearing_ && callback_ != nullptr) {
      callback = callback_;
      context = context_;
      ++in_flight_;
    }
  }

  bool locked_center = false;
  const bool query_ok =
      callback != nullptr && callback(context, &locked_center);
  if (callback != nullptr) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      --in_flight_;
    }
    condition_.notify_all();
  }
  const bool native_lock_active = query_ok && locked_center;
  native_lock_observed_ = native_lock_active;
  if (wait_for_native_unlock_after_right_drag_ && !native_lock_active) {
    wait_for_native_unlock_after_right_drag_ = false;
  }
  const bool client_active = query_ok && focused_;
  const bool native_capture_active = client_active && native_lock_active &&
                                     !wait_for_native_unlock_after_right_drag_;
  const bool right_drag_active = focused_ && right_button_held_;
  const bool capture_requested =
      (right_drag_active || native_capture_active) && !text_input_active;
  const bool cursor_visible =
      text_input_active || (!client_active && !right_drag_active);
  return Apply(capture_requested, cursor_visible);
}

bool WindowPointerCaptureOwner::OnRightButton(bool pressed,
                                              bool text_input_active) {
  if (pressed && !right_button_held_) {
    // nativeGetMainWindowIsMouseLockedCenter may start returning true as soon
    // as Roblox receives RMB down. Remember only a lock observed before the
    // drag so a stale post-release value cannot capture SDL again.
    native_lock_was_active_before_right_drag_ =
        native_lock_observed_ && !wait_for_native_unlock_after_right_drag_;
    shift_key_pressed_during_right_drag_ = false;
    discard_next_motion_after_right_drag_ = false;
  } else if (!pressed && right_button_held_) {
    const bool native_lock_should_survive_right_drag =
        native_lock_was_active_before_right_drag_ ||
        (shift_key_pressed_during_right_drag_ && native_lock_observed_);
    const bool transient_right_drag = !native_lock_should_survive_right_drag;
    wait_for_native_unlock_after_right_drag_ = transient_right_drag;
    discard_next_motion_after_right_drag_ = transient_right_drag;
    native_lock_was_active_before_right_drag_ = false;
    shift_key_pressed_during_right_drag_ = false;
  }
  right_button_held_ = focused_ && pressed;
  return Pump(text_input_active);
}

bool WindowPointerCaptureOwner::OnShiftKeyPressed(bool text_input_active) {
  if (!focused_ || !right_button_held_) {
    return true;
  }
  shift_key_pressed_during_right_drag_ = true;
  return Pump(text_input_active);
}

bool WindowPointerCaptureOwner::NeedsRightButtonReleaseRecovery(
    bool observed_pressed) const {
  return right_button_held_ && !observed_pressed;
}

bool WindowPointerCaptureOwner::ShouldDispatchMouseMotion() {
  const bool discard_motion = discard_next_motion_after_right_drag_;
  discard_next_motion_after_right_drag_ = false;
  return !discard_motion && !wait_for_native_unlock_after_right_drag_;
}

bool WindowPointerCaptureOwner::OnFocusGained() {
  focused_ = true;
  return true;
}

bool WindowPointerCaptureOwner::OnFocusLost() {
  focused_ = false;
  right_button_held_ = false;
  native_lock_observed_ = false;
  native_lock_was_active_before_right_drag_ = false;
  shift_key_pressed_during_right_drag_ = false;
  wait_for_native_unlock_after_right_drag_ = false;
  discard_next_motion_after_right_drag_ = false;
  return Apply(false, true);
}

bool WindowPointerCaptureOwner::Shutdown() {
  ClearQuery();
  focused_ = false;
  right_button_held_ = false;
  native_lock_observed_ = false;
  native_lock_was_active_before_right_drag_ = false;
  shift_key_pressed_during_right_drag_ = false;
  wait_for_native_unlock_after_right_drag_ = false;
  discard_next_motion_after_right_drag_ = false;
  return Apply(false, true);
}

bool WindowPointerCaptureOwner::Apply(bool capture, bool cursor_visible) {
  if (capture == captured_ && cursor_visible == cursor_visible_) {
    return true;
  }
  if (backend_ == nullptr) {
    captured_ = false;
    cursor_visible_ = true;
    return !capture && cursor_visible;
  }
  if (backend_->Apply(capture, cursor_visible)) {
    const bool capture_changed = capture != captured_;
    const bool cursor_changed = cursor_visible != cursor_visible_;
    captured_ = capture;
    cursor_visible_ = cursor_visible;
    if (capture_changed && capture) {
      std::fprintf(stderr, "  [input] SDL relative pointer capture enabled\n");
    } else if (capture_changed) {
      std::fprintf(stderr, "  [input] SDL relative pointer capture released\n");
    }
    if (cursor_changed) {
      std::fprintf(stderr, "  [input] SDL system cursor %s\n",
                   cursor_visible ? "shown" : "hidden");
    }
    return true;
  }
  backend_->Apply(false, true);
  captured_ = false;
  cursor_visible_ = true;
  return false;
}

bool SdlPointerCaptureBackend::Apply(bool relative_mode, bool cursor_visible) {
  auto* window = static_cast<SDL_Window*>(window_);
  if (window == nullptr) {
    return false;
  }

  if (relative_mode && !relative_mode_) {
    SDL_GetMouseState(&capture_anchor_x_, &capture_anchor_y_);
    capture_anchor_valid_ = true;
  }
  if (!relative_mode && relative_mode_ && capture_anchor_valid_) {
    // SDL requires the warp before relative mode is disabled. This preserves
    // the desktop pointer position across Roblox's transient RMB camera drag.
    SDL_WarpMouseInWindow(window, capture_anchor_x_, capture_anchor_y_);
  }
  if (relative_mode != relative_mode_ &&
      !SDL_SetWindowRelativeMouseMode(window, relative_mode)) {
    if (relative_mode) {
      capture_anchor_valid_ = false;
    }
    return false;
  }
  relative_mode_ = relative_mode;
  if (!relative_mode_) {
    capture_anchor_valid_ = false;
  }
  return cursor_visible ? SDL_ShowCursor() : SDL_HideCursor();
}

}  // namespace window
}  // namespace mocktail
