#include "window/window_pointer_capture_owner.h"

#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_video.h>

#include <cstdio>

namespace mocktail {
namespace window {

namespace {

// ClearQuery may be called by teardown reached from the native query itself.
// Such a call owns the in-flight marker and must not wait for its own stack to
// unwind.
thread_local WindowPointerCaptureOwner* t_pointer_query_owner = nullptr;

}  // namespace

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
  condition_.wait(lock, [this] {
    return in_flight_ == 0 || t_pointer_query_owner == this;
  });
  clearing_ = false;
  lock.unlock();
  condition_.notify_all();
}

void WindowPointerCaptureOwner::SetPointerModeChangeCallback(
    PointerModeChangeCallback callback, void* context) {
  std::lock_guard<std::mutex> lock(mutex_);
  pointer_mode_callback_ = callback;
  pointer_mode_context_ = context;
  // The observer registered after an earlier transition must still learn the
  // current mode, so re-arm the notification instead of assuming it is current.
  mode_notified_ = false;
}

void WindowPointerCaptureOwner::ClearPointerModeChangeCallback() {
  std::lock_guard<std::mutex> lock(mutex_);
  pointer_mode_callback_ = nullptr;
  pointer_mode_context_ = nullptr;
  mode_notified_ = false;
}

bool WindowPointerCaptureOwner::Pump(bool text_input_active) {
  if (!focused_) {
    // Losing focus releases the pointer, and every consumer of the effective
    // mode has to hear about it or it would keep pinning coordinates to a
    // crosshair nobody owns.
    const bool applied = Apply(false, true);
    NotifyPointerMode(false, text_input_active);
    return applied;
  }
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
      callback != nullptr &&
      [&] {
        WindowPointerCaptureOwner* previous_query_owner =
            t_pointer_query_owner;
        t_pointer_query_owner = this;
        struct ResetCallbackMarker {
          WindowPointerCaptureOwner* previous_query_owner;
          ~ResetCallbackMarker() {
            t_pointer_query_owner = previous_query_owner;
          }
        } reset_callback_marker{previous_query_owner};
        return callback(context, &locked_center);
      }();
  if (callback != nullptr) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      --in_flight_;
    }
    condition_.notify_all();
  }
  const bool native_lock_active = query_ok && locked_center;
  native_lock_observed_ = native_lock_active;
  // Diagnostics: the APK's lock-center state drives pointer confinement. If the
  // query fails, capture is never requested and the pointer stays free on
  // Wayland/X11 -- a common symptom when a third-party launcher does not expose
  // getMainWindowIsMouseLockedCenter.
  if (!query_ok) {
    std::fprintf(stderr,
                 "  [input] mouse-lock query failed; pointer capture will NOT "
                 "be requested (launcher/JNI gap?)\n");
  } else if (native_lock_active != native_lock_active_prev_) {
    std::fprintf(stderr, "  [input] APK mouse-lock-center state: %s\n",
                 native_lock_active ? "LOCKED" : "unlocked");
    native_lock_active_prev_ = native_lock_active;
  }

  // Text entry is a pointer mode, but it is not the host's decision to make:
  // Roblox keeps reporting lock-center for as long as its own Shift Lock is on,
  // and that report is what decides whether camera motion stays relative. Text
  // editing therefore only suppresses the host's *fallback* RMB camera capture
  // (see capture_requested below); it never overrides the guest's own lock.
  if (text_input_active != text_entry_active_) {
    std::fprintf(stderr, "  [input] pointer mode: text entry %s\n",
                 text_input_active ? "began" : "ended");
  }
  text_entry_active_ = text_input_active;

  if (wait_for_native_unlock_after_right_drag_ && !native_lock_active) {
    wait_for_native_unlock_after_right_drag_ = false;
  }
  const bool client_active = query_ok;
  const bool native_capture_active = client_active && native_lock_active;
  const bool right_drag_active = right_button_held_;
  // Roblox can keep Shift Lock active while chat has text focus. Honor that
  // native lock so camera motion stays relative and cannot leave the window.
  // Text editing only suppresses the host's fallback RMB camera capture.
  const bool capture_requested =
      native_capture_active || (right_drag_active && !text_input_active);
  // When capture is released, Roblox still draws its own cursor.
  // Show the system pointer only when the native client cannot provide one.
  const bool cursor_visible = !client_active && !capture_requested;
  const bool applied = Apply(capture_requested, cursor_visible);
  // Publish the effective mode as a single value so consumers never re-derive
  // it from raw coordinates or from the guest's own lagging lock report.
  NotifyPointerMode(capture_requested, text_input_active);
  return applied;
}

void WindowPointerCaptureOwner::NotifyPointerMode(bool captured,
                                                 bool text_entry_active) {
  if (mode_notified_ && captured == mode_captured_notified_ &&
      text_entry_active == mode_text_entry_notified_) {
    return;
  }
  mode_notified_ = true;
  mode_captured_notified_ = captured;
  mode_text_entry_notified_ = text_entry_active;
  if (pointer_mode_callback_ != nullptr) {
    pointer_mode_callback_(pointer_mode_context_, captured, text_entry_active);
  }
}

bool WindowPointerCaptureOwner::OnRightButton(bool pressed,
                                              bool text_input_active) {
  if (pressed) {
    focused_ = true;
  }
  right_button_held_ = pressed;
  return Pump(text_input_active);
}

bool WindowPointerCaptureOwner::OnShiftKeyPressed(bool text_input_active) {
  focused_ = true;
  return Pump(text_input_active);
}

bool WindowPointerCaptureOwner::NeedsRightButtonReleaseRecovery(
    bool /*observed_pressed*/) const {
  return false;
}

bool WindowPointerCaptureOwner::ShouldDispatchMouseMotion() {
  return true;
}

bool WindowPointerCaptureOwner::OnFocusGained(bool text_input_active) {
  focused_ = true;
  return Pump(text_input_active);
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
  ClearPointerModeChangeCallback();
  focused_ = false;
  right_button_held_ = false;
  text_entry_active_ = false;
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
    SDL_SetWindowMouseGrab(window, true);
  }
  if (!relative_mode && relative_mode_ && capture_anchor_valid_) {
    // SDL requires the warp before relative mode is disabled. This preserves
    // the desktop pointer position across Roblox's transient RMB camera drag.
    SDL_WarpMouseInWindow(window, capture_anchor_x_, capture_anchor_y_);
    SDL_SetWindowMouseGrab(window, false);
  }
  if (relative_mode != relative_mode_ &&
      !SDL_SetWindowRelativeMouseMode(window, relative_mode)) {
    if (relative_mode) {
      capture_anchor_valid_ = false;
      SDL_SetWindowMouseGrab(window, false);
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
