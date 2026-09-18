#ifndef MOCKTAIL_WINDOW_WINDOW_POINTER_CAPTURE_OWNER_H_
#define MOCKTAIL_WINDOW_WINDOW_POINTER_CAPTURE_OWNER_H_

#include <condition_variable>
#include <cstddef>
#include <mutex>

namespace mocktail {
namespace window {

using MouseLockQueryCallback = bool (*)(void* context, bool* locked_center);
// Notifies observers when the *effective* pointer mode changes. This owner is
// the single authority for that mode: it resolves the guest's lock-center
// query, the host's text-entry session, and transient right-drag capture, so
// consumers (the input router) never re-derive it from raw coordinates or from
// the guest's lagging report. Push-based, so it fires only on transitions.
using PointerModeChangeCallback = void (*)(void* context, bool captured,
                                           bool text_entry_active);

class PointerCaptureBackend {
 public:
  virtual ~PointerCaptureBackend() = default;
  virtual bool Apply(bool relative_mode, bool cursor_visible) = 0;
};

class WindowPointerCaptureOwner final {
 public:
  explicit WindowPointerCaptureOwner(PointerCaptureBackend* backend);
  ~WindowPointerCaptureOwner();

  WindowPointerCaptureOwner(const WindowPointerCaptureOwner&) = delete;
  WindowPointerCaptureOwner& operator=(const WindowPointerCaptureOwner&) =
      delete;

  bool RegisterQuery(MouseLockQueryCallback callback, void* context);
  void ClearQuery();
  void SetPointerModeChangeCallback(PointerModeChangeCallback callback,
                                    void* context);
  void ClearPointerModeChangeCallback();
  // Resolves the effective pointer mode for this frame. text_input_active is
  // the host's text-entry state; see Pump() in the implementation for how the
  // guest's lock-center query and the host's fallback RMB drag compose. The
  // resulting mode is published through the change callback.
  bool Pump(bool text_input_active);
  // Android's lock-center query does not represent desktop RMB camera drag,
  // so the SDL owner tracks that transient capture source independently.
  bool OnRightButton(bool pressed, bool text_input_active);
  // Shift can enable Roblox's persistent mouse lock while a transient RMB
  // camera drag is active. Refresh the native state after dispatching that
  // key so the subsequent RMB release preserves the intentional lock.
  bool OnShiftKeyPressed(bool text_input_active);
  bool NeedsRightButtonReleaseRecovery(bool observed_pressed) const;
  // Mirrors Android's captured-pointer listener: the motion that releases a
  // transient capture is consumed instead of leaking one final camera delta.
  bool ShouldDispatchMouseMotion();
  // Focus loss reveals the system cursor. Regaining focus must re-apply the
  // pointer state immediately: Pump() runs at the top of the frame, before
  // SDL_PollEvent delivers the focus event, so deferring the re-apply leaves
  // the desktop arrow on screen for at least one whole frame.
  bool OnFocusGained(bool text_input_active);
  bool OnFocusLost();
  bool Shutdown();

  bool captured() const { return captured_; }
  bool cursor_visible() const { return cursor_visible_; }

 private:
  bool Apply(bool capture, bool cursor_visible);
  void NotifyPointerMode(bool captured, bool text_entry_active);

  PointerCaptureBackend* backend_ = nullptr;
  std::mutex mutex_;
  std::condition_variable condition_;
  MouseLockQueryCallback callback_ = nullptr;
  void* context_ = nullptr;
  std::size_t in_flight_ = 0;
  bool clearing_ = false;
  PointerModeChangeCallback pointer_mode_callback_ = nullptr;
  void* pointer_mode_context_ = nullptr;
  bool native_lock_active_prev_ = false;
  // Last text-entry state that was published. Tracked so the transition can be
  // logged and the notification deduplicated; it never overrides the guest's
  // own lock-center report.
  bool text_entry_active_ = false;
  bool mode_notified_ = false;
  bool mode_captured_notified_ = false;
  bool mode_text_entry_notified_ = false;
  bool focused_ = true;
  bool right_button_held_ = false;
  bool native_lock_observed_ = false;
  bool native_lock_was_active_before_right_drag_ = false;
  bool shift_key_pressed_during_right_drag_ = false;
  bool wait_for_native_unlock_after_right_drag_ = false;
  bool discard_next_motion_after_right_drag_ = false;
  bool captured_ = false;
  bool cursor_visible_ = true;
};

class SdlPointerCaptureBackend final : public PointerCaptureBackend {
 public:
  explicit SdlPointerCaptureBackend(void* window) : window_(window) {}

  bool Apply(bool relative_mode, bool cursor_visible) override;

 private:
  void* window_ = nullptr;
  float capture_anchor_x_ = 0.0f;
  float capture_anchor_y_ = 0.0f;
  bool relative_mode_ = false;
  bool capture_anchor_valid_ = false;
};

}  // namespace window
}  // namespace mocktail

#endif  // MOCKTAIL_WINDOW_WINDOW_POINTER_CAPTURE_OWNER_H_
