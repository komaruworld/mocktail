#include "runtime/roblox_input_native_adapter.h"

#include <utf8proc.h>

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace mocktail {
namespace runtime {
namespace {

constexpr char kNativeInputClassName[] =
    "com/roblox/engine/jni/NativeInputInterface";
constexpr char kNativeGlClassName[] = "com/roblox/engine/jni/NativeGLInterface";

Status FailedPrecondition(const char* message) {
  return Status::Error(
      StatusCode::kFailedPrecondition,
      message != nullptr ? message : "input adapter precondition failed");
}

Status Unavailable(const char* message) {
  return Status::Error(StatusCode::kUnavailable,
                       message != nullptr ? message : "input is unavailable");
}

Status Unsupported(const char* message) {
  return Status::Error(StatusCode::kUnsupported,
                       message != nullptr ? message : "input is unsupported");
}

jstring NewJavaString(JNIEnv* env, const char* utf8, std::size_t size) {
  if (env == nullptr || (utf8 == nullptr && size != 0)) {
    return nullptr;
  }
  std::vector<jchar> utf16;
  utf16.reserve(size);
  std::size_t offset = 0;
  while (offset < size) {
    utf8proc_int32_t codepoint = 0;
    const utf8proc_ssize_t count = utf8proc_iterate(
        reinterpret_cast<const utf8proc_uint8_t*>(utf8 + offset),
        static_cast<utf8proc_ssize_t>(size - offset), &codepoint);
    if (count <= 0) {
      return nullptr;
    }
    if (codepoint <= 0xffff) {
      utf16.push_back(static_cast<jchar>(codepoint));
    } else {
      codepoint -= 0x10000;
      utf16.push_back(static_cast<jchar>(0xd800 + (codepoint >> 10)));
      utf16.push_back(static_cast<jchar>(0xdc00 + (codepoint & 0x3ff)));
    }
    offset += static_cast<std::size_t>(count);
  }
  return env->NewString(utf16.data(), static_cast<jsize>(utf16.size()));
}

}  // namespace

RobloxInputNativeAdapter::RobloxInputNativeAdapter(
    JniEnvironmentProvider environment, RobloxInputSymbols symbols)
    : environment_(environment), symbols_(symbols) {}

RobloxInputNativeAdapter::~RobloxInputNativeAdapter() { Release(); }

Status RobloxInputNativeAdapter::Initialize() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (initialized_) {
    return FailedPrecondition("Roblox input adapter is already initialized");
  }
  if (!environment_.valid()) {
    return Unavailable("Roblox input JavaVM is unavailable");
  }
  if (symbols_.pass_mouse_move == nullptr ||
      symbols_.pass_mouse_button == nullptr ||
      symbols_.pass_mouse_wheel == nullptr || symbols_.pass_input == nullptr ||
      symbols_.pass_key_event == nullptr ||
      symbols_.get_main_window_is_mouse_locked_center == nullptr ||
      symbols_.sync_textbox_text_and_cursor_position2 == nullptr ||
      symbols_.get_text_box_info == nullptr || symbols_.pass_text == nullptr ||
      symbols_.return_pressed_from_on_screen_keyboard == nullptr ||
      symbols_.release_focus == nullptr) {
    return Unavailable("Roblox interactive input export table is incomplete");
  }

  JNIEnv* env = nullptr;
  Status status = environment_.Acquire(&env);
  if (!status.ok()) {
    return status;
  }

  jclass local_input_class = env->FindClass(kNativeInputClassName);
  jclass local_gl_class = env->FindClass(kNativeGlClassName);
  if (local_input_class != nullptr) {
    native_input_class_ =
        static_cast<jclass>(env->NewGlobalRef(local_input_class));
    env->DeleteLocalRef(local_input_class);
  }
  if (local_gl_class != nullptr) {
    native_gl_class_ = static_cast<jclass>(env->NewGlobalRef(local_gl_class));
    env->DeleteLocalRef(local_gl_class);
  }
  status = CheckJniException(env, "retain Roblox input JNI classes");
  if (!status.ok() || native_input_class_ == nullptr ||
      native_gl_class_ == nullptr) {
    if (native_input_class_ != nullptr) {
      env->DeleteGlobalRef(native_input_class_);
      native_input_class_ = nullptr;
    }
    if (native_gl_class_ != nullptr) {
      env->DeleteGlobalRef(native_gl_class_);
      native_gl_class_ = nullptr;
    }
    return status.ok() ? Unavailable("Roblox input JNI classes are unavailable")
                       : status;
  }

  initialized_ = true;
  return Status::Ok();
}

Status RobloxInputNativeAdapter::Release() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!initialized_ && native_input_class_ == nullptr &&
      native_gl_class_ == nullptr) {
    return Status::Ok();
  }
  JNIEnv* env = nullptr;
  Status status = environment_.Acquire(&env);
  if (!status.ok()) {
    return status;
  }
  if (native_gl_class_ != nullptr) {
    env->DeleteGlobalRef(native_gl_class_);
    native_gl_class_ = nullptr;
  }
  if (native_input_class_ != nullptr) {
    env->DeleteGlobalRef(native_input_class_);
    native_input_class_ = nullptr;
  }
  initialized_ = false;
  return CheckJniException(env, "release Roblox input JNI classes");
}

bool RobloxInputNativeAdapter::initialized() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return initialized_;
}

Status RobloxInputNativeAdapter::GetMainWindowIsMouseLockedCenter(
    bool* locked_center) {
  if (locked_center == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "mouse lock output is required");
  }
  *locked_center = false;
  std::lock_guard<std::mutex> lock(mutex_);
  JNIEnv* env = nullptr;
  Status status = RequireReadyLocked(&env);
  if (!status.ok()) {
    return status;
  }
  *locked_center = symbols_.get_main_window_is_mouse_locked_center(
                       env, native_input_class_) == JNI_TRUE;
  return CheckJniException(env, "nativeGetMainWindowIsMouseLockedCenter");
}

Status RobloxInputNativeAdapter::QueryCurrentTextBoxInfo(
    RobloxNativeTextBoxInfoQueryResult* result) {
  if (result == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "TextBox info output is required");
  }
  *result = {};
  std::lock_guard<std::mutex> lock(mutex_);
  JNIEnv* env = nullptr;
  Status status = RequireReadyLocked(&env);
  if (!status.ok()) {
    return status;
  }
  return QueryRobloxNativeTextBoxInfo(env, native_gl_class_,
                                      symbols_.get_text_box_info, result);
}

RobloxInputSink RobloxInputNativeAdapter::Sink() {
  RobloxInputSink sink = {this,
                          &RobloxInputNativeAdapter::MouseMoveCallback,
                          &RobloxInputNativeAdapter::MouseButtonCallback,
                          &RobloxInputNativeAdapter::MouseWheelCallback,
                          &RobloxInputNativeAdapter::TouchCallback,
                          &RobloxInputNativeAdapter::KeyCallback,
                          {}};
  sink.text = {this, &RobloxInputNativeAdapter::SyncTextCallback,
               &RobloxInputNativeAdapter::PassTextCallback,
               &RobloxInputNativeAdapter::ReturnPressedCallback,
               &RobloxInputNativeAdapter::ReleaseFocusCallback};
  return sink;
}

Status RobloxInputNativeAdapter::MouseMoveCallback(void* context, float x,
                                                   float y, float delta_x,
                                                   float delta_y) {
  return static_cast<RobloxInputNativeAdapter*>(context)->MouseMove(
      x, y, delta_x, delta_y);
}

Status RobloxInputNativeAdapter::MouseButtonCallback(void* context, float x,
                                                     float y, bool pressed,
                                                     int32_t button) {
  return static_cast<RobloxInputNativeAdapter*>(context)->MouseButton(
      x, y, pressed, button);
}

Status RobloxInputNativeAdapter::MouseWheelCallback(void* context, float x,
                                                    float y, float delta_y) {
  return static_cast<RobloxInputNativeAdapter*>(context)->MouseWheel(x, y,
                                                                     delta_y);
}

Status RobloxInputNativeAdapter::TouchCallback(void* context,
                                               int32_t pointer_id, float x,
                                               float y, int32_t action,
                                               int32_t width, int32_t height) {
  return static_cast<RobloxInputNativeAdapter*>(context)->Touch(
      pointer_id, x, y, action, width, height);
}

Status RobloxInputNativeAdapter::KeyCallback(void* context, bool pressed,
                                             int32_t scan_code,
                                             int32_t key_code, bool repeat) {
  return static_cast<RobloxInputNativeAdapter*>(context)->Key(
      pressed, scan_code, key_code, repeat);
}

Status RobloxInputNativeAdapter::SyncTextCallback(void* context,
                                                  const char* utf8,
                                                  std::size_t size,
                                                  int32_t cursor_utf16) {
  return static_cast<RobloxInputNativeAdapter*>(context)->SyncText(
      utf8, size, cursor_utf16);
}

Status RobloxInputNativeAdapter::PassTextCallback(
    void* context, int64_t textbox_handle, const char* utf8, std::size_t size,
    bool finished, int32_t cursor_utf16) {
  return static_cast<RobloxInputNativeAdapter*>(context)->PassText(
      textbox_handle, utf8, size, finished, cursor_utf16);
}

Status RobloxInputNativeAdapter::ReturnPressedCallback(void* context,
                                                       int64_t textbox_handle) {
  return static_cast<RobloxInputNativeAdapter*>(context)->ReturnPressed(
      textbox_handle);
}

Status RobloxInputNativeAdapter::ReleaseFocusCallback(void* context,
                                                      int64_t textbox_handle) {
  return static_cast<RobloxInputNativeAdapter*>(context)->ReleaseFocus(
      textbox_handle);
}

Status RobloxInputNativeAdapter::MouseMove(float x, float y, float delta_x,
                                           float delta_y) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (symbols_.pass_mouse_move == nullptr) {
    return Unsupported("nativePassMouseMove is unavailable");
  }
  JNIEnv* env = nullptr;
  Status status = RequireReadyLocked(&env);
  if (!status.ok()) {
    return status;
  }
  symbols_.pass_mouse_move(env, native_input_class_, x, y, delta_x, delta_y);
  return CheckJniException(env, "nativePassMouseMove");
}

Status RobloxInputNativeAdapter::MouseButton(float x, float y, bool pressed,
                                             int32_t button) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (symbols_.pass_mouse_button == nullptr) {
    return Unsupported("nativePassMouseButton is unavailable");
  }
  JNIEnv* env = nullptr;
  Status status = RequireReadyLocked(&env);
  if (!status.ok()) {
    return status;
  }
  symbols_.pass_mouse_button(env, native_input_class_, x, y,
                             pressed ? JNI_TRUE : JNI_FALSE, button);
  return CheckJniException(env, "nativePassMouseButton");
}

Status RobloxInputNativeAdapter::MouseWheel(float x, float y, float delta_y) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (symbols_.pass_mouse_wheel == nullptr) {
    return Unsupported("nativePassMouseWheel is unavailable");
  }
  JNIEnv* env = nullptr;
  Status status = RequireReadyLocked(&env);
  if (!status.ok()) {
    return status;
  }
  symbols_.pass_mouse_wheel(env, native_input_class_, x, y, delta_y);
  return CheckJniException(env, "nativePassMouseWheel");
}

Status RobloxInputNativeAdapter::Touch(int32_t pointer_id, float x, float y,
                                       int32_t action, int32_t width,
                                       int32_t height) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (symbols_.pass_input == nullptr) {
    return Unsupported("nativePassInput is unavailable");
  }
  JNIEnv* env = nullptr;
  Status status = RequireReadyLocked(&env);
  if (!status.ok()) {
    return status;
  }
  symbols_.pass_input(env, native_input_class_, pointer_id, x, y, action, width,
                      height);
  return CheckJniException(env, "nativePassInput");
}

Status RobloxInputNativeAdapter::Key(bool pressed, int32_t scan_code,
                                     int32_t key_code, bool repeat) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (symbols_.pass_key_event == nullptr) {
    return Unsupported("nativePassKeyEvent is unavailable");
  }
  JNIEnv* env = nullptr;
  Status status = RequireReadyLocked(&env);
  if (!status.ok()) {
    return status;
  }
  symbols_.pass_key_event(env, native_gl_class_, pressed ? JNI_TRUE : JNI_FALSE,
                          scan_code, key_code, repeat ? JNI_TRUE : JNI_FALSE);
  return CheckJniException(env, "nativePassKeyEvent");
}

Status RobloxInputNativeAdapter::SyncText(const char* utf8, std::size_t size,
                                          int32_t cursor_utf16) {
  std::lock_guard<std::mutex> lock(mutex_);
  JNIEnv* env = nullptr;
  Status status = RequireReadyLocked(&env);
  if (!status.ok()) {
    return status;
  }
  jstring text = NewJavaString(env, utf8, size);
  if (text == nullptr) {
    status = CheckJniException(env, "allocate Roblox TextBox Java string");
    return status.ok()
               ? Unavailable("failed to allocate Roblox TextBox Java string")
               : status;
  }
  symbols_.sync_textbox_text_and_cursor_position2(env, native_gl_class_, text,
                                                  cursor_utf16);
  env->DeleteLocalRef(text);
  return CheckJniException(env, "syncTextboxTextAndCursorPosition2");
}

Status RobloxInputNativeAdapter::PassText(int64_t textbox_handle,
                                          const char* utf8, std::size_t size,
                                          bool finished, int32_t cursor_utf16) {
  std::lock_guard<std::mutex> lock(mutex_);
  JNIEnv* env = nullptr;
  Status status = RequireReadyLocked(&env);
  if (!status.ok()) {
    return status;
  }
  jstring text = NewJavaString(env, utf8, size);
  if (text == nullptr) {
    status = CheckJniException(env, "allocate Roblox TextBox Java string");
    return status.ok()
               ? Unavailable("failed to allocate Roblox TextBox Java string")
               : status;
  }
  symbols_.pass_text(env, native_gl_class_, static_cast<jlong>(textbox_handle),
                     text, finished ? JNI_TRUE : JNI_FALSE, cursor_utf16);
  env->DeleteLocalRef(text);
  return CheckJniException(env, "nativePassText");
}

Status RobloxInputNativeAdapter::ReturnPressed(int64_t textbox_handle) {
  std::lock_guard<std::mutex> lock(mutex_);
  JNIEnv* env = nullptr;
  Status status = RequireReadyLocked(&env);
  if (!status.ok()) {
    return status;
  }
  symbols_.return_pressed_from_on_screen_keyboard(
      env, native_gl_class_, static_cast<jlong>(textbox_handle));
  return CheckJniException(env, "nativeReturnPressedFromOnScreenKeyboard");
}

Status RobloxInputNativeAdapter::ReleaseFocus(int64_t textbox_handle) {
  std::lock_guard<std::mutex> lock(mutex_);
  JNIEnv* env = nullptr;
  Status status = RequireReadyLocked(&env);
  if (!status.ok()) {
    return status;
  }
  symbols_.release_focus(env, native_gl_class_,
                         static_cast<jlong>(textbox_handle));
  return CheckJniException(env, "nativeReleaseFocus");
}

Status RobloxInputNativeAdapter::CheckJniException(
    JNIEnv* env, const char* operation) const {
  if (env == nullptr) {
    return Unavailable("JNIEnv disappeared during Roblox input dispatch");
  }
  if (env->ExceptionCheck() == JNI_FALSE) {
    return Status::Ok();
  }
  env->ExceptionClear();
  std::string message = operation != nullptr ? operation : "Roblox input";
  message += " raised a JNI exception";
  return Status::Error(StatusCode::kPlatformError, std::move(message));
}

Status RobloxInputNativeAdapter::RequireReadyLocked(JNIEnv** env) const {
  if (env == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "JNIEnv output is required");
  }
  *env = nullptr;
  if (!initialized_ || native_input_class_ == nullptr ||
      native_gl_class_ == nullptr) {
    return FailedPrecondition("Roblox input adapter is not initialized");
  }
  return environment_.Acquire(env);
}

RobloxInputRuntime::RobloxInputRuntime(JniEnvironmentProvider environment,
                                       RobloxInputSymbols symbols,
                                       RobloxTextDisplaySink display_sink,
                                       platform::TextClipboard* clipboard)
    : adapter_(environment, symbols),
      router_(adapter_.Sink(), display_sink, clipboard) {}

RobloxInputRuntime::~RobloxInputRuntime() { Deactivate(); }

Status RobloxInputRuntime::Initialize(RobloxInputViewport viewport,
                                      bool initially_focused) {
  Status status = adapter_.Initialize();
  if (!status.ok()) {
    return status;
  }
  status = router_.Activate(viewport, initially_focused);
  if (!status.ok()) {
    adapter_.Release();
    return status;
  }
  std::fprintf(stderr,
               "  [input] typed production input ready: mouse=1 touch=1 "
               "keyboard=1 text=1\n");
  return Status::Ok();
}

Status RobloxInputRuntime::Deactivate() {
  Status router_status = router_.Deactivate();
  Status adapter_status = adapter_.Release();
  return !router_status.ok() ? router_status : adapter_status;
}

Status RobloxInputRuntime::BeginTextFocusSession(
    RobloxTextFocusSession session) {
  return router_.BeginTextFocusSession(std::move(session));
}

Status RobloxInputRuntime::EndTextFocusSession(int64_t textbox_handle,
                                               uint64_t generation,
                                               bool notify_native) {
  return router_.EndTextFocusSession(textbox_handle, generation, notify_native);
}

Status RobloxInputRuntime::ReplaceFocusedTextFromEngine(
    uint64_t generation, std::string authoritative_utf8) {
  return router_.ReplaceFocusedTextFromEngine(generation,
                                              std::move(authoritative_utf8));
}

Status RobloxInputRuntime::QueryCurrentTextBoxInfo(
    RobloxNativeTextBoxInfoQueryResult* result) {
  return adapter_.QueryCurrentTextBoxInfo(result);
}

Status RobloxInputRuntime::UpdateTextFocusProperties(
    uint64_t generation, const RobloxTextFocusProperties& properties) {
  return router_.UpdateTextFocusProperties(generation, properties);
}

RobloxInputDispatchResult RobloxInputRuntime::HandleEvent(
    const platform::PlatformEvent& event) {
  RobloxInputDispatchResult result = router_.HandleEvent(event);
  LogFirstDispatch(result);
  return result;
}

RobloxInputSnapshot RobloxInputRuntime::Snapshot() const {
  return router_.Snapshot();
}

Status RobloxInputRuntime::GetMainWindowIsMouseLockedCenter(
    bool* locked_center) {
  return adapter_.GetMainWindowIsMouseLockedCenter(locked_center);
}

void RobloxInputRuntime::PlatformEventCallback(
    void* context, const platform::PlatformEvent& event) {
  if (context == nullptr) {
    return;
  }
  static_cast<RobloxInputRuntime*>(context)->HandleEvent(event);
}

void RobloxInputRuntime::LogFirstDispatch(
    const RobloxInputDispatchResult& result) {
  std::lock_guard<std::mutex> lock(log_mutex_);
  if (result.dispatched() &&
      result.kind == RobloxInputEventKind::kMouseButton &&
      !logged_mouse_button_) {
    logged_mouse_button_ = true;
    std::fprintf(stderr,
                 "  [input] first mouse button reached Roblox native input\n");
  } else if (result.dispatched() &&
             result.kind == RobloxInputEventKind::kTouch && !logged_touch_) {
    logged_touch_ = true;
    std::fprintf(stderr,
                 "  [input] first touch event reached Roblox native input\n");
  } else if (result.dispatched() &&
             result.kind == RobloxInputEventKind::kKeyboard &&
             !logged_keyboard_) {
    logged_keyboard_ = true;
    std::fprintf(
        stderr, "  [input] first keyboard event reached Roblox native input\n");
  } else if (result.dispatched() &&
             result.kind == RobloxInputEventKind::kText && !logged_text_) {
    logged_text_ = true;
    std::fprintf(stderr,
                 "  [input] first live text update reached Roblox native "
                 "TextBox\n");
  } else if (result.kind == RobloxInputEventKind::kText &&
             result.state == RobloxInputDispatchState::kIgnoredUnsupported &&
             !logged_text_unsupported_) {
    logged_text_unsupported_ = true;
    std::fprintf(stderr,
                 "  [input] text event rejected: no focused Roblox TextBox "
                 "handle\n");
  } else if (result.state == RobloxInputDispatchState::kNativeError &&
             !logged_native_error_) {
    logged_native_error_ = true;
    std::fprintf(stderr, "  [input] native dispatch failed: %s\n",
                 result.status.message().c_str());
  }
}

}  // namespace runtime
}  // namespace mocktail
