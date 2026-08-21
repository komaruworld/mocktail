#include "runtime/roblox_input_router.h"

#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>

#include <algorithm>
#include <utility>

namespace mocktail {
namespace runtime {
namespace {

constexpr int32_t kAndroidTouchDown = 0;
constexpr int32_t kAndroidTouchMove = 1;
constexpr int32_t kAndroidTouchUp = 2;
constexpr int32_t kMaximumAndroidPointers = 32;

Status InvalidArgument(const char* message) {
  return Status::Error(StatusCode::kInvalidArgument,
                       message != nullptr ? message : "invalid input event");
}

Status Unsupported(const char* message) {
  return Status::Error(
      StatusCode::kUnsupported,
      message != nullptr ? message : "unsupported input event");
}

RobloxInputDispatchResult Result(RobloxInputDispatchState state,
                                 RobloxInputEventKind kind,
                                 Status status = Status::Ok()) {
  return {state, kind, std::move(status)};
}

}  // namespace

AndroidKeyMapping MapSdlKeyToAndroid(uint32_t sdl_scancode) {
  switch (static_cast<SDL_Scancode>(sdl_scancode)) {
    case SDL_SCANCODE_A:
      return {30, 29};
    case SDL_SCANCODE_B:
      return {48, 30};
    case SDL_SCANCODE_C:
      return {46, 31};
    case SDL_SCANCODE_D:
      return {32, 32};
    case SDL_SCANCODE_E:
      return {18, 33};
    case SDL_SCANCODE_F:
      return {33, 34};
    case SDL_SCANCODE_G:
      return {34, 35};
    case SDL_SCANCODE_H:
      return {35, 36};
    case SDL_SCANCODE_I:
      return {23, 37};
    case SDL_SCANCODE_J:
      return {36, 38};
    case SDL_SCANCODE_K:
      return {37, 39};
    case SDL_SCANCODE_L:
      return {38, 40};
    case SDL_SCANCODE_M:
      return {50, 41};
    case SDL_SCANCODE_N:
      return {49, 42};
    case SDL_SCANCODE_O:
      return {24, 43};
    case SDL_SCANCODE_P:
      return {25, 44};
    case SDL_SCANCODE_Q:
      return {16, 45};
    case SDL_SCANCODE_R:
      return {19, 46};
    case SDL_SCANCODE_S:
      return {31, 47};
    case SDL_SCANCODE_T:
      return {20, 48};
    case SDL_SCANCODE_U:
      return {22, 49};
    case SDL_SCANCODE_V:
      return {47, 50};
    case SDL_SCANCODE_W:
      return {17, 51};
    case SDL_SCANCODE_X:
      return {45, 52};
    case SDL_SCANCODE_Y:
      return {21, 53};
    case SDL_SCANCODE_Z:
      return {44, 54};
    case SDL_SCANCODE_1:
      return {2, 8};
    case SDL_SCANCODE_2:
      return {3, 9};
    case SDL_SCANCODE_3:
      return {4, 10};
    case SDL_SCANCODE_4:
      return {5, 11};
    case SDL_SCANCODE_5:
      return {6, 12};
    case SDL_SCANCODE_6:
      return {7, 13};
    case SDL_SCANCODE_7:
      return {8, 14};
    case SDL_SCANCODE_8:
      return {9, 15};
    case SDL_SCANCODE_9:
      return {10, 16};
    case SDL_SCANCODE_0:
      return {11, 7};
    case SDL_SCANCODE_RETURN:
      return {28, 66};
    case SDL_SCANCODE_ESCAPE:
      return {1, 111};
    case SDL_SCANCODE_BACKSPACE:
      return {14, 67};
    case SDL_SCANCODE_TAB:
      return {15, 61};
    case SDL_SCANCODE_SPACE:
      return {57, 62};
    case SDL_SCANCODE_MINUS:
      return {12, 69};
    case SDL_SCANCODE_EQUALS:
      return {13, 70};
    case SDL_SCANCODE_LEFTBRACKET:
      return {26, 71};
    case SDL_SCANCODE_RIGHTBRACKET:
      return {27, 72};
    case SDL_SCANCODE_BACKSLASH:
      return {43, 73};
    case SDL_SCANCODE_SEMICOLON:
      return {39, 74};
    case SDL_SCANCODE_APOSTROPHE:
      return {40, 75};
    case SDL_SCANCODE_GRAVE:
      return {41, 68};
    case SDL_SCANCODE_COMMA:
      return {51, 55};
    case SDL_SCANCODE_PERIOD:
      return {52, 56};
    case SDL_SCANCODE_SLASH:
      return {53, 76};
    case SDL_SCANCODE_CAPSLOCK:
      return {58, 115};
    case SDL_SCANCODE_F1:
      return {59, 131};
    case SDL_SCANCODE_F2:
      return {60, 132};
    case SDL_SCANCODE_F3:
      return {61, 133};
    case SDL_SCANCODE_F4:
      return {62, 134};
    case SDL_SCANCODE_F5:
      return {63, 135};
    case SDL_SCANCODE_F6:
      return {64, 136};
    case SDL_SCANCODE_F7:
      return {65, 137};
    case SDL_SCANCODE_F8:
      return {66, 138};
    case SDL_SCANCODE_F9:
      return {67, 139};
    case SDL_SCANCODE_F10:
      return {68, 140};
    case SDL_SCANCODE_F11:
      return {87, 141};
    case SDL_SCANCODE_F12:
      return {88, 142};
    case SDL_SCANCODE_PRINTSCREEN:
      return {99, 120};
    case SDL_SCANCODE_SCROLLLOCK:
      return {70, 116};
    case SDL_SCANCODE_PAUSE:
      return {119, 121};
    case SDL_SCANCODE_INSERT:
      return {110, 124};
    case SDL_SCANCODE_HOME:
      return {102, 122};
    case SDL_SCANCODE_PAGEUP:
      return {104, 92};
    case SDL_SCANCODE_DELETE:
      return {111, 112};
    case SDL_SCANCODE_END:
      return {107, 123};
    case SDL_SCANCODE_PAGEDOWN:
      return {109, 93};
    case SDL_SCANCODE_RIGHT:
      return {106, 22};
    case SDL_SCANCODE_LEFT:
      return {105, 21};
    case SDL_SCANCODE_DOWN:
      return {108, 20};
    case SDL_SCANCODE_UP:
      return {103, 19};
    case SDL_SCANCODE_NUMLOCKCLEAR:
      return {69, 143};
    case SDL_SCANCODE_KP_DIVIDE:
      return {98, 154};
    case SDL_SCANCODE_KP_MULTIPLY:
      return {55, 155};
    case SDL_SCANCODE_KP_MINUS:
      return {74, 156};
    case SDL_SCANCODE_KP_PLUS:
      return {78, 157};
    case SDL_SCANCODE_KP_ENTER:
      return {96, 160};
    case SDL_SCANCODE_KP_1:
      return {79, 145};
    case SDL_SCANCODE_KP_2:
      return {80, 146};
    case SDL_SCANCODE_KP_3:
      return {81, 147};
    case SDL_SCANCODE_KP_4:
      return {75, 148};
    case SDL_SCANCODE_KP_5:
      return {76, 149};
    case SDL_SCANCODE_KP_6:
      return {77, 150};
    case SDL_SCANCODE_KP_7:
      return {71, 151};
    case SDL_SCANCODE_KP_8:
      return {72, 152};
    case SDL_SCANCODE_KP_9:
      return {73, 153};
    case SDL_SCANCODE_KP_0:
      return {82, 144};
    case SDL_SCANCODE_KP_PERIOD:
      return {83, 158};
    case SDL_SCANCODE_LCTRL:
      return {29, 113};
    case SDL_SCANCODE_LSHIFT:
      return {42, 59};
    case SDL_SCANCODE_LALT:
      return {56, 57};
    case SDL_SCANCODE_LGUI:
      return {125, 117};
    case SDL_SCANCODE_RCTRL:
      return {97, 114};
    case SDL_SCANCODE_RSHIFT:
      return {54, 60};
    case SDL_SCANCODE_RALT:
      return {100, 58};
    case SDL_SCANCODE_RGUI:
      return {126, 118};
    case SDL_SCANCODE_APPLICATION:
      return {127, 82};
    default:
      return {};
  }
}

int32_t MapSdlMouseButtonToAndroid(uint8_t sdl_button) {
  switch (sdl_button) {
    case SDL_BUTTON_LEFT:
      return 0;
    case SDL_BUTTON_RIGHT:
      return 1;
    case SDL_BUTTON_MIDDLE:
      return 3;
    case SDL_BUTTON_X1:
      return 7;
    case SDL_BUTTON_X2:
      return 15;
    default:
      return -1;
  }
}

RobloxInputRouter::RobloxInputRouter(RobloxInputSink sink,
                                     RobloxTextDisplaySink display_sink,
                                     platform::TextClipboard* clipboard)
    : sink_(sink), text_editor_(sink.text, display_sink, clipboard) {}

Status RobloxInputRouter::Activate(RobloxInputViewport viewport,
                                   bool initially_focused) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (snapshot_.active) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "Roblox input router is already active");
  }
  if (!viewport.valid()) {
    return InvalidArgument("input viewport dimensions must be positive");
  }
  snapshot_ = {};
  snapshot_.active = true;
  snapshot_.focused = initially_focused;
  snapshot_.viewport = viewport;
  active_mouse_buttons_.clear();
  active_touches_.clear();
  active_keys_.clear();
  mouse_x_ = 0.0f;
  mouse_y_ = 0.0f;
  return Status::Ok();
}

Status RobloxInputRouter::Deactivate() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!snapshot_.active) {
    return Status::Ok();
  }
  Status status = ReleasePressedInputsLocked();
  const RobloxTextEditorSnapshot text = text_editor_.Snapshot();
  if (text.focused) {
    Status text_status = text_editor_.EndFocusSession(text.textbox_handle,
                                                      text.generation, true);
    if (status.ok()) {
      status = std::move(text_status);
    }
  }
  snapshot_.active = false;
  snapshot_.focused = false;
  snapshot_.active_mouse_buttons = 0;
  snapshot_.active_touches = 0;
  snapshot_.active_keys = 0;
  return status;
}

Status RobloxInputRouter::BeginTextFocusSession(
    RobloxTextFocusSession session) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!snapshot_.active) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "Roblox input router is inactive");
  }
  return text_editor_.BeginFocusSession(std::move(session));
}

Status RobloxInputRouter::EndTextFocusSession(int64_t textbox_handle,
                                              uint64_t generation,
                                              bool notify_native) {
  std::lock_guard<std::mutex> lock(mutex_);
  return text_editor_.EndFocusSession(textbox_handle, generation,
                                      notify_native);
}

Status RobloxInputRouter::ReplaceFocusedTextFromEngine(
    uint64_t generation, std::string authoritative_utf8) {
  std::lock_guard<std::mutex> lock(mutex_);
  return text_editor_.ReplaceFocusedTextFromEngine(
      generation, std::move(authoritative_utf8));
}

Status RobloxInputRouter::UpdateTextFocusProperties(
    uint64_t generation, const RobloxTextFocusProperties& properties) {
  std::lock_guard<std::mutex> lock(mutex_);
  return text_editor_.UpdateFocusProperties(generation, properties);
}

RobloxInputDispatchResult RobloxInputRouter::HandleEvent(
    const platform::PlatformEvent& event) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!snapshot_.active) {
    return Result(RobloxInputDispatchState::kIgnoredInactive,
                  RobloxInputEventKind::kNone);
  }

  if (const auto* resized =
          std::get_if<platform::WindowResizedEvent>(&event.payload)) {
    RobloxInputViewport viewport = {
        resized->logical_width, resized->logical_height, resized->pixel_width,
        resized->pixel_height};
    if (!viewport.valid()) {
      return Result(RobloxInputDispatchState::kRejectedInvalid,
                    RobloxInputEventKind::kViewport,
                    InvalidArgument("resized input viewport is invalid"));
    }
    snapshot_.viewport = viewport;
    return Result(RobloxInputDispatchState::kStateUpdated,
                  RobloxInputEventKind::kViewport);
  }

  if (const auto* focus =
          std::get_if<platform::WindowFocusEvent>(&event.payload)) {
    if (!focus->focused && snapshot_.focused) {
      Status status = ReleasePressedInputsLocked();
      const RobloxTextEditorSnapshot text = text_editor_.Snapshot();
      if (text.focused) {
        Status text_status = text_editor_.EndFocusSession(
            text.textbox_handle, text.generation, true);
        if (status.ok()) {
          status = std::move(text_status);
        }
      }
      snapshot_.focused = false;
      if (!status.ok()) {
        ++snapshot_.native_errors;
        return Result(RobloxInputDispatchState::kNativeError,
                      RobloxInputEventKind::kFocus, std::move(status));
      }
    } else {
      snapshot_.focused = focus->focused;
    }
    return Result(RobloxInputDispatchState::kStateUpdated,
                  RobloxInputEventKind::kFocus);
  }

  RobloxInputEventKind kind = RobloxInputEventKind::kNone;
  if (std::holds_alternative<platform::MouseMotionEvent>(event.payload)) {
    kind = RobloxInputEventKind::kMouseMotion;
  } else if (std::holds_alternative<platform::MouseButtonEvent>(
                 event.payload)) {
    kind = RobloxInputEventKind::kMouseButton;
  } else if (std::holds_alternative<platform::MouseWheelEvent>(event.payload)) {
    kind = RobloxInputEventKind::kMouseWheel;
  } else if (std::holds_alternative<platform::TouchEvent>(event.payload)) {
    kind = RobloxInputEventKind::kTouch;
  } else if (std::holds_alternative<platform::KeyEvent>(event.payload)) {
    kind = RobloxInputEventKind::kKeyboard;
  } else if (std::holds_alternative<platform::TextInputEvent>(event.payload) ||
             std::holds_alternative<platform::TextEditingEvent>(
                 event.payload)) {
    kind = RobloxInputEventKind::kText;
  }
  if (kind == RobloxInputEventKind::kNone) {
    return Result(RobloxInputDispatchState::kIgnoredUnsupported, kind,
                  Unsupported("platform event has no Roblox input route"));
  }
  if (!snapshot_.focused) {
    return Result(RobloxInputDispatchState::kIgnoredUnfocused, kind);
  }

  if (const auto* motion =
          std::get_if<platform::MouseMotionEvent>(&event.payload)) {
    return HandleMouseMotionLocked(*motion);
  }
  if (const auto* button =
          std::get_if<platform::MouseButtonEvent>(&event.payload)) {
    return HandleMouseButtonLocked(*button);
  }
  if (const auto* wheel =
          std::get_if<platform::MouseWheelEvent>(&event.payload)) {
    return HandleMouseWheelLocked(*wheel);
  }
  if (const auto* touch = std::get_if<platform::TouchEvent>(&event.payload)) {
    return HandleTouchLocked(*touch);
  }
  if (const auto* key = std::get_if<platform::KeyEvent>(&event.payload)) {
    RobloxTextEditResult text_result = text_editor_.HandleKey(*key);
    if (text_result.handled) {
      if (!text_result.status.ok()) {
        ++snapshot_.native_errors;
        return Result(RobloxInputDispatchState::kNativeError,
                      RobloxInputEventKind::kText,
                      std::move(text_result.status));
      }
      if (text_result.dispatched) {
        ++snapshot_.text_events;
      }
      return Result(text_result.dispatched
                        ? RobloxInputDispatchState::kDispatched
                        : RobloxInputDispatchState::kStateUpdated,
                    RobloxInputEventKind::kText);
    }
    return HandleKeyLocked(*key);
  }
  RobloxTextEditResult text_result;
  if (const auto* input =
          std::get_if<platform::TextInputEvent>(&event.payload)) {
    text_result = text_editor_.HandleTextInput(*input);
  } else if (const auto* editing =
                 std::get_if<platform::TextEditingEvent>(&event.payload)) {
    text_result = text_editor_.HandleTextEditing(*editing);
  }
  if (!text_result.handled) {
    ++snapshot_.ignored_text_events;
    return Result(RobloxInputDispatchState::kIgnoredUnsupported,
                  RobloxInputEventKind::kText,
                  Unsupported("text input has no focused Roblox TextBox"));
  }
  if (!text_result.status.ok()) {
    ++snapshot_.native_errors;
    return Result(RobloxInputDispatchState::kNativeError,
                  RobloxInputEventKind::kText, std::move(text_result.status));
  }
  if (text_result.dispatched) {
    ++snapshot_.text_events;
  }
  return Result(text_result.dispatched
                    ? RobloxInputDispatchState::kDispatched
                    : RobloxInputDispatchState::kStateUpdated,
                RobloxInputEventKind::kText);
}

RobloxInputSnapshot RobloxInputRouter::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_;
}

RobloxInputDispatchResult RobloxInputRouter::HandleMouseMotionLocked(
    const platform::MouseMotionEvent& event) {
  if (sink_.mouse_move == nullptr) {
    return Result(RobloxInputDispatchState::kIgnoredUnsupported,
                  RobloxInputEventKind::kMouseMotion,
                  Unsupported("native mouse motion is unavailable"));
  }
  mouse_x_ = event.x;
  mouse_y_ = event.y;
  return NativeResultLocked(sink_.mouse_move(sink_.context, event.x, event.y,
                                             event.delta_x, event.delta_y),
                            RobloxInputEventKind::kMouseMotion);
}

RobloxInputDispatchResult RobloxInputRouter::HandleMouseButtonLocked(
    const platform::MouseButtonEvent& event) {
  if (sink_.mouse_button == nullptr) {
    return Result(RobloxInputDispatchState::kIgnoredUnsupported,
                  RobloxInputEventKind::kMouseButton,
                  Unsupported("native mouse buttons are unavailable"));
  }
  const int32_t android_button = MapSdlMouseButtonToAndroid(event.button);
  if (android_button < 0) {
    return Result(RobloxInputDispatchState::kIgnoredUnsupported,
                  RobloxInputEventKind::kMouseButton,
                  Unsupported("SDL mouse button has no Android mapping"));
  }
  mouse_x_ = event.x;
  mouse_y_ = event.y;
  Status status = sink_.mouse_button(sink_.context, event.x, event.y,
                                     event.pressed, android_button);
  if (status.ok()) {
    const auto active = std::find(active_mouse_buttons_.begin(),
                                  active_mouse_buttons_.end(), android_button);
    if (event.pressed && active == active_mouse_buttons_.end()) {
      active_mouse_buttons_.push_back(android_button);
    } else if (!event.pressed && active != active_mouse_buttons_.end()) {
      active_mouse_buttons_.erase(active);
    }
    snapshot_.active_mouse_buttons = active_mouse_buttons_.size();
  }
  return NativeResultLocked(std::move(status),
                            RobloxInputEventKind::kMouseButton);
}

RobloxInputDispatchResult RobloxInputRouter::HandleMouseWheelLocked(
    const platform::MouseWheelEvent& event) {
  if (sink_.mouse_wheel == nullptr || event.delta_y == 0.0f) {
    return Result(RobloxInputDispatchState::kIgnoredUnsupported,
                  RobloxInputEventKind::kMouseWheel,
                  Unsupported("vertical native mouse wheel is unavailable"));
  }
  mouse_x_ = std::max(0.0f, event.mouse_x);
  mouse_y_ = std::max(0.0f, event.mouse_y);
  return NativeResultLocked(
      sink_.mouse_wheel(sink_.context, mouse_x_, mouse_y_, event.delta_y),
      RobloxInputEventKind::kMouseWheel);
}

RobloxInputDispatchResult RobloxInputRouter::HandleTouchLocked(
    const platform::TouchEvent& event) {
  if (sink_.touch == nullptr) {
    return Result(RobloxInputDispatchState::kIgnoredUnsupported,
                  RobloxInputEventKind::kTouch,
                  Unsupported("native touch input is unavailable"));
  }
  const float x = std::clamp(event.x, 0.0f, 1.0f) *
                  static_cast<float>(snapshot_.viewport.logical_width);
  const float y = std::clamp(event.y, 0.0f, 1.0f) *
                  static_cast<float>(snapshot_.viewport.logical_height);
  auto active = FindTouchIteratorLocked(event.touch_id, event.finger_id);
  int32_t action = kAndroidTouchMove;
  int32_t pointer_id = -1;

  if (event.action == platform::TouchEvent::Action::kDown) {
    if (active != active_touches_.end()) {
      return Result(RobloxInputDispatchState::kRejectedInvalid,
                    RobloxInputEventKind::kTouch,
                    InvalidArgument("duplicate touch down was rejected"));
    }
    pointer_id = AllocatePointerIdLocked();
    if (pointer_id < 0) {
      return Result(RobloxInputDispatchState::kRejectedInvalid,
                    RobloxInputEventKind::kTouch,
                    InvalidArgument("Android pointer limit was reached"));
    }
    action = kAndroidTouchDown;
  } else {
    if (active == active_touches_.end()) {
      return Result(RobloxInputDispatchState::kRejectedInvalid,
                    RobloxInputEventKind::kTouch,
                    InvalidArgument("touch event has no active pointer"));
    }
    pointer_id = active->pointer_id;
    action = event.action == platform::TouchEvent::Action::kMove
                 ? kAndroidTouchMove
                 : kAndroidTouchUp;
  }

  Status status = sink_.touch(sink_.context, pointer_id, x, y, action,
                              snapshot_.viewport.logical_width,
                              snapshot_.viewport.logical_height);
  if (event.action == platform::TouchEvent::Action::kDown && status.ok()) {
    active_touches_.push_back(
        {event.touch_id, event.finger_id, pointer_id, x, y});
  } else if (active != active_touches_.end()) {
    if (event.action == platform::TouchEvent::Action::kMove) {
      if (status.ok()) {
        active->x = x;
        active->y = y;
      }
    } else {
      active_touches_.erase(active);
    }
  }
  snapshot_.active_touches = active_touches_.size();
  return NativeResultLocked(std::move(status), RobloxInputEventKind::kTouch);
}

RobloxInputDispatchResult RobloxInputRouter::HandleKeyLocked(
    const platform::KeyEvent& event) {
  if (sink_.key == nullptr) {
    return Result(RobloxInputDispatchState::kIgnoredUnsupported,
                  RobloxInputEventKind::kKeyboard,
                  Unsupported("native hardware keyboard is unavailable"));
  }
  const AndroidKeyMapping android = MapSdlKeyToAndroid(event.scancode);
  if (!android.valid()) {
    return Result(RobloxInputDispatchState::kIgnoredUnsupported,
                  RobloxInputEventKind::kKeyboard,
                  Unsupported("SDL key has no Android mapping"));
  }
  Status status = sink_.key(sink_.context, event.pressed, android.scan_code,
                            android.key_code, event.repeat);
  if (status.ok()) {
    auto active = FindKeyIteratorLocked(event.scancode);
    if (event.pressed && active == active_keys_.end()) {
      active_keys_.push_back({event.scancode, android});
    } else if (!event.pressed && active != active_keys_.end()) {
      active_keys_.erase(active);
    }
    snapshot_.active_keys = active_keys_.size();
  }
  return NativeResultLocked(std::move(status), RobloxInputEventKind::kKeyboard);
}

Status RobloxInputRouter::ReleasePressedInputsLocked() {
  Status first_error = Status::Ok();
  for (int32_t button : active_mouse_buttons_) {
    if (sink_.mouse_button == nullptr) {
      continue;
    }
    Status status =
        sink_.mouse_button(sink_.context, mouse_x_, mouse_y_, false, button);
    if (status.ok()) {
      ++snapshot_.mouse_events;
    } else if (first_error.ok()) {
      first_error = std::move(status);
    }
  }
  active_mouse_buttons_.clear();

  for (const ActiveTouch& touch : active_touches_) {
    if (sink_.touch == nullptr) {
      continue;
    }
    Status status = sink_.touch(
        sink_.context, touch.pointer_id, touch.x, touch.y, kAndroidTouchUp,
        snapshot_.viewport.logical_width, snapshot_.viewport.logical_height);
    if (status.ok()) {
      ++snapshot_.touch_events;
    } else if (first_error.ok()) {
      first_error = std::move(status);
    }
  }
  active_touches_.clear();

  for (const ActiveKey& key : active_keys_) {
    if (sink_.key == nullptr) {
      continue;
    }
    Status status = sink_.key(sink_.context, false, key.android.scan_code,
                              key.android.key_code, false);
    if (status.ok()) {
      ++snapshot_.keyboard_events;
    } else if (first_error.ok()) {
      first_error = std::move(status);
    }
  }
  active_keys_.clear();
  snapshot_.active_mouse_buttons = 0;
  snapshot_.active_touches = 0;
  snapshot_.active_keys = 0;
  return first_error;
}

int32_t RobloxInputRouter::AllocatePointerIdLocked() const {
  for (int32_t candidate = 0; candidate < kMaximumAndroidPointers;
       ++candidate) {
    const auto used =
        std::find_if(active_touches_.begin(), active_touches_.end(),
                     [candidate](const ActiveTouch& touch) {
                       return touch.pointer_id == candidate;
                     });
    if (used == active_touches_.end()) {
      return candidate;
    }
  }
  return -1;
}

std::vector<RobloxInputRouter::ActiveTouch>::iterator
RobloxInputRouter::FindTouchIteratorLocked(int64_t touch_id,
                                           int64_t finger_id) {
  return std::find_if(active_touches_.begin(), active_touches_.end(),
                      [touch_id, finger_id](const ActiveTouch& touch) {
                        return touch.touch_id == touch_id &&
                               touch.finger_id == finger_id;
                      });
}

std::vector<RobloxInputRouter::ActiveKey>::iterator
RobloxInputRouter::FindKeyIteratorLocked(uint32_t sdl_scancode) {
  return std::find_if(active_keys_.begin(), active_keys_.end(),
                      [sdl_scancode](const ActiveKey& key) {
                        return key.sdl_scancode == sdl_scancode;
                      });
}

RobloxInputDispatchResult RobloxInputRouter::NativeResultLocked(
    Status status, RobloxInputEventKind kind) {
  if (!status.ok()) {
    ++snapshot_.native_errors;
    return Result(RobloxInputDispatchState::kNativeError, kind,
                  std::move(status));
  }
  switch (kind) {
    case RobloxInputEventKind::kMouseMotion:
    case RobloxInputEventKind::kMouseButton:
    case RobloxInputEventKind::kMouseWheel:
      ++snapshot_.mouse_events;
      break;
    case RobloxInputEventKind::kTouch:
      ++snapshot_.touch_events;
      break;
    case RobloxInputEventKind::kKeyboard:
      ++snapshot_.keyboard_events;
      break;
    default:
      break;
  }
  return Result(RobloxInputDispatchState::kDispatched, kind);
}

const char* RobloxInputDispatchStateName(RobloxInputDispatchState state) {
  switch (state) {
    case RobloxInputDispatchState::kDispatched:
      return "Dispatched";
    case RobloxInputDispatchState::kStateUpdated:
      return "StateUpdated";
    case RobloxInputDispatchState::kIgnoredInactive:
      return "IgnoredInactive";
    case RobloxInputDispatchState::kIgnoredUnfocused:
      return "IgnoredUnfocused";
    case RobloxInputDispatchState::kIgnoredUnsupported:
      return "IgnoredUnsupported";
    case RobloxInputDispatchState::kRejectedInvalid:
      return "RejectedInvalid";
    case RobloxInputDispatchState::kNativeError:
      return "NativeError";
  }
  return "Unknown";
}

const char* RobloxInputEventKindName(RobloxInputEventKind kind) {
  switch (kind) {
    case RobloxInputEventKind::kNone:
      return "None";
    case RobloxInputEventKind::kMouseMotion:
      return "MouseMotion";
    case RobloxInputEventKind::kMouseButton:
      return "MouseButton";
    case RobloxInputEventKind::kMouseWheel:
      return "MouseWheel";
    case RobloxInputEventKind::kTouch:
      return "Touch";
    case RobloxInputEventKind::kKeyboard:
      return "Keyboard";
    case RobloxInputEventKind::kText:
      return "Text";
    case RobloxInputEventKind::kFocus:
      return "Focus";
    case RobloxInputEventKind::kViewport:
      return "Viewport";
  }
  return "Unknown";
}

}  // namespace runtime
}  // namespace mocktail
