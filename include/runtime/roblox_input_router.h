#ifndef MOCKTAIL_RUNTIME_ROBLOX_INPUT_ROUTER_H_
#define MOCKTAIL_RUNTIME_ROBLOX_INPUT_ROUTER_H_

#include <cstdint>
#include <mutex>
#include <vector>

#include "mocktail/platform/platform_runtime.h"
#include "mocktail/status.h"
#include "runtime/roblox_text_editor.h"

namespace mocktail {
namespace runtime {

struct RobloxInputViewport {
  int32_t logical_width = 0;
  int32_t logical_height = 0;
  int32_t pixel_width = 0;
  int32_t pixel_height = 0;

  bool valid() const {
    return logical_width > 0 && logical_height > 0 && pixel_width > 0 &&
           pixel_height > 0;
  }
};

struct AndroidKeyMapping {
  int32_t scan_code = 0;
  int32_t key_code = 0;

  bool valid() const { return scan_code > 0 && key_code > 0; }
};

// Maps SDL's USB scancode to the Linux evdev scan code and Android KeyEvent
// keycode observed by the APK. Unmapped keys are rejected rather than passed
// through with an ABI-incompatible numeric value.
AndroidKeyMapping MapSdlKeyToAndroid(uint32_t sdl_scancode);

// Mirrors the APK's MotionEvent.getActionButton() - 1 conversion. SDL button
// numbers are not numerically compatible with Android button bit values.
int32_t MapSdlMouseButtonToAndroid(uint8_t sdl_button);

struct RobloxInputSink {
  using MouseMoveFn = Status (*)(void* context, float x, float y, float delta_x,
                                 float delta_y);
  using MouseButtonFn = Status (*)(void* context, float x, float y,
                                   bool pressed, int32_t button);
  using MouseWheelFn = Status (*)(void* context, float x, float y,
                                  float delta_y);
  using TouchFn = Status (*)(void* context, int32_t pointer_id, float x,
                             float y, int32_t action, int32_t width,
                             int32_t height);
  using KeyFn = Status (*)(void* context, bool pressed, int32_t scan_code,
                           int32_t key_code, bool repeat);

  void* context = nullptr;
  MouseMoveFn mouse_move = nullptr;
  MouseButtonFn mouse_button = nullptr;
  MouseWheelFn mouse_wheel = nullptr;
  TouchFn touch = nullptr;
  KeyFn key = nullptr;
  RobloxTextSink text;
};

enum class RobloxInputEventKind {
  kNone = 0,
  kMouseMotion,
  kMouseButton,
  kMouseWheel,
  kTouch,
  kKeyboard,
  kText,
  kFocus,
  kViewport,
};

enum class RobloxInputDispatchState {
  kDispatched = 0,
  kStateUpdated,
  kIgnoredInactive,
  kIgnoredUnfocused,
  kIgnoredUnsupported,
  kRejectedInvalid,
  kNativeError,
};

struct RobloxInputDispatchResult {
  RobloxInputDispatchState state = RobloxInputDispatchState::kStateUpdated;
  RobloxInputEventKind kind = RobloxInputEventKind::kNone;
  Status status = Status::Ok();

  bool dispatched() const {
    return state == RobloxInputDispatchState::kDispatched;
  }
};

struct RobloxInputSnapshot {
  bool active = false;
  bool focused = false;
  RobloxInputViewport viewport;
  uint64_t mouse_events = 0;
  uint64_t touch_events = 0;
  uint64_t keyboard_events = 0;
  uint64_t ignored_text_events = 0;
  uint64_t text_events = 0;
  uint64_t native_errors = 0;
  uint32_t active_mouse_buttons = 0;
  uint32_t active_touches = 0;
  uint32_t active_keys = 0;
};

// Stateful translation boundary from SDL platform events to the exact
// Android/Roblox native input ABI. It owns no JNI or SDL objects.
class RobloxInputRouter final {
 public:
  explicit RobloxInputRouter(RobloxInputSink sink,
                             RobloxTextDisplaySink display_sink = {},
                             platform::TextClipboard* clipboard = nullptr);

  RobloxInputRouter(const RobloxInputRouter&) = delete;
  RobloxInputRouter& operator=(const RobloxInputRouter&) = delete;

  Status Activate(RobloxInputViewport viewport, bool initially_focused);
  Status Deactivate();
  Status BeginTextFocusSession(RobloxTextFocusSession session);
  Status EndTextFocusSession(int64_t textbox_handle, uint64_t generation,
                             bool notify_native);
  Status ReplaceFocusedTextFromEngine(uint64_t generation,
                                      std::string authoritative_utf8);
  Status UpdateTextFocusProperties(uint64_t generation,
                                   const RobloxTextFocusProperties& properties);
  RobloxInputDispatchResult HandleEvent(const platform::PlatformEvent& event);
  RobloxInputSnapshot Snapshot() const;

 private:
  struct ActiveTouch {
    int64_t touch_id = 0;
    int64_t finger_id = 0;
    int32_t pointer_id = -1;
    float x = 0.0f;
    float y = 0.0f;
  };

  struct ActiveKey {
    uint32_t sdl_scancode = 0;
    AndroidKeyMapping android;
  };

  RobloxInputDispatchResult HandleMouseMotionLocked(
      const platform::MouseMotionEvent& event);
  RobloxInputDispatchResult HandleMouseButtonLocked(
      const platform::MouseButtonEvent& event);
  RobloxInputDispatchResult HandleMouseWheelLocked(
      const platform::MouseWheelEvent& event);
  RobloxInputDispatchResult HandleTouchLocked(
      const platform::TouchEvent& event);
  RobloxInputDispatchResult HandleKeyLocked(const platform::KeyEvent& event);
  Status ReleasePressedInputsLocked();
  int32_t AllocatePointerIdLocked() const;
  std::vector<ActiveTouch>::iterator FindTouchIteratorLocked(int64_t touch_id,
                                                             int64_t finger_id);
  std::vector<ActiveKey>::iterator FindKeyIteratorLocked(uint32_t sdl_scancode);
  RobloxInputDispatchResult NativeResultLocked(Status status,
                                               RobloxInputEventKind kind);

  const RobloxInputSink sink_;
  RobloxTextEditor text_editor_;
  mutable std::mutex mutex_;
  RobloxInputSnapshot snapshot_;
  std::vector<int32_t> active_mouse_buttons_;
  std::vector<ActiveTouch> active_touches_;
  std::vector<ActiveKey> active_keys_;
  float mouse_x_ = 0.0f;
  float mouse_y_ = 0.0f;
};

const char* RobloxInputDispatchStateName(RobloxInputDispatchState state);
const char* RobloxInputEventKindName(RobloxInputEventKind kind);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_ROBLOX_INPUT_ROUTER_H_
