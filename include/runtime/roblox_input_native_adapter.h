#ifndef MOCKTAIL_RUNTIME_ROBLOX_INPUT_NATIVE_ADAPTER_H_
#define MOCKTAIL_RUNTIME_ROBLOX_INPUT_NATIVE_ADAPTER_H_

#include <jni.h>

#include <cstddef>
#include <mutex>
#include <string>

#include "mocktail/platform/platform_runtime.h"
#include "mocktail/status.h"
#include "runtime/roblox_capability_resolver.h"
#include "runtime/roblox_game_session_native_adapter.h"
#include "runtime/roblox_input_router.h"
#include "runtime/roblox_native_text_box_info_reader.h"

namespace mocktail {
namespace runtime {

// Owns the JNI class references used by the exported NativeInputInterface and
// NativeGLInterface entrypoints. Every operation acquires a thread-local JNIEnv
// synchronously and rejects calls after Release.
class RobloxInputNativeAdapter final {
 public:
  RobloxInputNativeAdapter(JniEnvironmentProvider environment,
                           RobloxInputSymbols symbols);
  ~RobloxInputNativeAdapter();

  RobloxInputNativeAdapter(const RobloxInputNativeAdapter&) = delete;
  RobloxInputNativeAdapter& operator=(const RobloxInputNativeAdapter&) = delete;

  Status Initialize();
  Status Release();
  bool initialized() const;
  Status GetMainWindowIsMouseLockedCenter(bool* locked_center);
  Status QueryCurrentTextBoxInfo(RobloxNativeTextBoxInfoQueryResult* result);
  RobloxInputSink Sink();

 private:
  static Status MouseMoveCallback(void* context, float x, float y,
                                  float delta_x, float delta_y);
  static Status MouseButtonCallback(void* context, float x, float y,
                                    bool pressed, int32_t button);
  static Status MouseWheelCallback(void* context, float x, float y,
                                   float delta_y);
  static Status TouchCallback(void* context, int32_t pointer_id, float x,
                              float y, int32_t action, int32_t width,
                              int32_t height);
  static Status KeyCallback(void* context, bool pressed, int32_t scan_code,
                            int32_t key_code, bool repeat);
  static Status SyncTextCallback(void* context, const char* utf8,
                                 std::size_t size, int32_t cursor_utf16);
  static Status PassTextCallback(void* context, int64_t textbox_handle,
                                 const char* utf8, std::size_t size,
                                 bool finished, int32_t cursor_utf16);
  static Status ReturnPressedCallback(void* context, int64_t textbox_handle);
  static Status ReleaseFocusCallback(void* context, int64_t textbox_handle);

  Status MouseMove(float x, float y, float delta_x, float delta_y);
  Status MouseButton(float x, float y, bool pressed, int32_t button);
  Status MouseWheel(float x, float y, float delta_y);
  Status Touch(int32_t pointer_id, float x, float y, int32_t action,
               int32_t width, int32_t height);
  Status Key(bool pressed, int32_t scan_code, int32_t key_code, bool repeat);
  Status SyncText(const char* utf8, std::size_t size, int32_t cursor_utf16);
  Status PassText(int64_t textbox_handle, const char* utf8, std::size_t size,
                  bool finished, int32_t cursor_utf16);
  Status ReturnPressed(int64_t textbox_handle);
  Status ReleaseFocus(int64_t textbox_handle);
  Status CheckJniException(JNIEnv* env, const char* operation) const;
  Status RequireReadyLocked(JNIEnv** env) const;

  const JniEnvironmentProvider environment_;
  const RobloxInputSymbols symbols_;
  mutable std::mutex mutex_;
  jclass native_input_class_ = nullptr;
  jclass native_gl_class_ = nullptr;
  bool initialized_ = false;
};

// Production owner used by the shared SDL event observer. Text input remains
// fail-closed until gameActivity_showKeyboard supplies the real
// focused TextBox handle required by nativePassText.
class RobloxInputRuntime final {
 public:
  RobloxInputRuntime(JniEnvironmentProvider environment,
                     RobloxInputSymbols symbols,
                     RobloxTextDisplaySink display_sink = {},
                     platform::TextClipboard* clipboard = nullptr);
  ~RobloxInputRuntime();

  RobloxInputRuntime(const RobloxInputRuntime&) = delete;
  RobloxInputRuntime& operator=(const RobloxInputRuntime&) = delete;

  Status Initialize(RobloxInputViewport viewport, bool initially_focused);
  Status Deactivate();
  Status BeginTextFocusSession(RobloxTextFocusSession session);
  Status EndTextFocusSession(int64_t textbox_handle, uint64_t generation,
                             bool notify_native);
  Status ReplaceFocusedTextFromEngine(uint64_t generation,
                                      std::string authoritative_utf8);
  Status QueryCurrentTextBoxInfo(RobloxNativeTextBoxInfoQueryResult* result);
  Status UpdateTextFocusProperties(uint64_t generation,
                                   const RobloxTextFocusProperties& properties);
  RobloxInputDispatchResult HandleEvent(const platform::PlatformEvent& event);
  RobloxInputSnapshot Snapshot() const;
  Status GetMainWindowIsMouseLockedCenter(bool* locked_center);

  static void PlatformEventCallback(void* context,
                                    const platform::PlatformEvent& event);

 private:
  void LogFirstDispatch(const RobloxInputDispatchResult& result);

  RobloxInputNativeAdapter adapter_;
  RobloxInputRouter router_;
  mutable std::mutex log_mutex_;
  bool logged_mouse_button_ = false;
  bool logged_touch_ = false;
  bool logged_keyboard_ = false;
  bool logged_text_ = false;
  bool logged_text_unsupported_ = false;
  bool logged_native_error_ = false;
};

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_ROBLOX_INPUT_NATIVE_ADAPTER_H_
