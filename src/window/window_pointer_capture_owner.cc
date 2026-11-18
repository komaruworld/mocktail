#include "window/window_pointer_capture_owner.h"

#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <cmath>
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

bool WindowPointerCaptureOwner::Pump(bool text_input_active) {
  if (!focused_) {
    return Apply(false, true, PointerReleasePosition::kUnchanged);
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
  // Roblox can keep Shift Lock active while chat has text focus. Honor that
  // native lock so camera motion stays relative and cannot leave the window.
  // Text editing only suppresses the host's fallback RMB camera capture.
  const bool right_drag_active = right_button_held_ && !text_input_active;
  const bool capture_requested = native_lock_active || right_drag_active;
  if (!captured_ && capture_requested) {
    release_position_ = right_drag_active
                            ? PointerReleasePosition::kDragStart
                            : PointerReleasePosition::kWindowCenter;
  } else if (native_lock_active && !right_button_held_ &&
             shift_key_pressed_during_right_drag_) {
    // Shift Lock took over the drag. An ordinary drag can also leave the
    // native query locked for one more pump after button-up.
    release_position_ = PointerReleasePosition::kWindowCenter;
  }
  // When capture is released, Roblox still draws its own cursor.
  // Show the system pointer only when the native client cannot provide one.
  const bool cursor_visible = !query_ok && !capture_requested;
  return Apply(capture_requested, cursor_visible, release_position_);
}

bool WindowPointerCaptureOwner::OnRightButton(
    bool pressed, bool text_input_active,
    std::optional<PointerPosition> position) {
  if (pressed) {
    focused_ = true;
    if (!right_button_held_ && !captured_) right_drag_anchor_ = position;
  }
  right_button_held_ = pressed;
  const bool applied = Pump(text_input_active);
  if (!pressed) {
    right_drag_anchor_.reset();
    shift_key_pressed_during_right_drag_ = false;
  }
  return applied;
}

bool WindowPointerCaptureOwner::OnShiftKeyPressed(bool text_input_active) {
  focused_ = true;
  if (right_button_held_) shift_key_pressed_during_right_drag_ = true;
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
  right_drag_anchor_.reset();
  shift_key_pressed_during_right_drag_ = false;
  return Apply(false, true, PointerReleasePosition::kUnchanged);
}

bool WindowPointerCaptureOwner::Shutdown() {
  ClearQuery();
  return OnFocusLost();
}

bool WindowPointerCaptureOwner::Apply(
    bool capture, bool cursor_visible, PointerReleasePosition release_position) {
  if (state_valid_ && capture == captured_ && cursor_visible == cursor_visible_) {
    return true;
  }
  if (backend_ == nullptr) {
    captured_ = false;
    cursor_visible_ = true;
    return !capture && cursor_visible;
  }
  if (backend_->Apply(
          {capture, cursor_visible, right_drag_anchor_, release_position})) {
    state_valid_ = true;
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
  state_valid_ = backend_->Apply({});
  captured_ = false;
  cursor_visible_ = true;
  return false;
}

bool SdlPointerCaptureBackend::Apply(const PointerCaptureRequest& request) {
  auto* window = static_cast<SDL_Window*>(window_);
  if (window == nullptr) {
    return false;
  }

  if (request.relative_mode && !relative_mode_) {
    if (!SDL_SetWindowMouseGrab(window, true)) {
      return false;
    }
    if (request.release_position == PointerReleasePosition::kDragStart) {
      drag_start_ = request.anchor;
      if (!drag_start_) {
        drag_start_.emplace();
        SDL_GetMouseState(&drag_start_->x, &drag_start_->y);
      }
    }
  }
  if (!request.relative_mode && relative_mode_ &&
      request.release_position != PointerReleasePosition::kUnchanged &&
      SDL_GetKeyboardFocus() == window) {
    // Set SDL's absolute position before unlocking so the next button event
    // agrees with the cursor that Roblox draws.
    int width = 0;
    int height = 0;
    if (SDL_GetWindowSize(window, &width, &height) && width > 0 && height > 0) {
      if (request.release_position == PointerReleasePosition::kWindowCenter) {
        SDL_WarpMouseInWindow(window, width / 2.0f, height / 2.0f);
      } else if (drag_start_ && std::isfinite(drag_start_->x) &&
                 std::isfinite(drag_start_->y)) {
        const float x = std::clamp(drag_start_->x, 0.0f, width - 1.0f);
        const float y = std::clamp(drag_start_->y, 0.0f, height - 1.0f);
        SDL_WarpMouseInWindow(window, x, y);
      }
    }
  }
  if (request.relative_mode != relative_mode_ &&
      !SDL_SetWindowRelativeMouseMode(window, request.relative_mode)) {
    if (request.relative_mode) {
      drag_start_.reset();
      SDL_SetWindowMouseGrab(window, false);
    }
    return false;
  }
  relative_mode_ = request.relative_mode;
  if (!relative_mode_) {
    drag_start_.reset();
  }
  if (!request.relative_mode && !SDL_SetWindowMouseGrab(window, false)) return false;
  return request.cursor_visible ? SDL_ShowCursor() : SDL_HideCursor();
}

}  // namespace window
}  // namespace mocktail
