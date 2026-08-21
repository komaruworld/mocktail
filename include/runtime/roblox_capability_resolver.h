#ifndef MOCKTAIL_RUNTIME_ROBLOX_CAPABILITY_RESOLVER_H_
#define MOCKTAIL_RUNTIME_ROBLOX_CAPABILITY_RESOLVER_H_

#include <jni.h>

#include <string>
#include <vector>

namespace mocktail {
namespace runtime {

// Non-owning callback used to look up exported symbols. The resolver invokes
// callbacks synchronously and never retains the callback or its context.
class RobloxSymbolLookup {
 public:
  using Callback = void* (*)(void* context, const char* symbol_name);

  RobloxSymbolLookup() = default;
  RobloxSymbolLookup(Callback callback, void* context)
      : callback_(callback), context_(context) {}

  bool valid() const { return callback_ != nullptr; }
  void* Find(const char* symbol_name) const;

 private:
  Callback callback_ = nullptr;
  void* context_ = nullptr;
};

struct RobloxCapabilityRequirements {
  // A real windowed startup also requires the app-surface update entrypoint.
  bool windowed = false;
};

using JniOnLoadFn = jint (*)(JavaVM* vm, void* reserved);
using NativeGameGlobalInitFn = void (*)(JNIEnv* env, jclass clazz);
using NativeUpdateAdapterInitFn = void (*)(JNIEnv* env, jclass clazz);
using NativeAppBridgeV2InitWithParamsFn = void (*)(JNIEnv* env, jclass clazz,
                                                   jobject init_params);
using NativeAppBridgeStartLuaAppDmFn = void (*)(JNIEnv* env, jclass clazz);
using NativeAppBridgeV2StartAppWithParamsFn =
    void (*)(JNIEnv* env, jclass clazz, jobject start_app_params);
using NativeAppBridgeV2UpdateSurfaceAppFn = void (*)(JNIEnv* env, jclass clazz,
                                                     jobject surface,
                                                     jobject platform_params);
using NativeCallMessagesFromMainThreadFn = void (*)(JNIEnv* env, jclass clazz);
using NativeAppBridgeV2StartGameWithParamFn =
    jint (*)(JNIEnv* env, jclass clazz, jobject start_game_params);
using NativeAppBridgeV2UpdateSurfaceGameFn = void (*)(JNIEnv* env, jclass clazz,
                                                      jobject surface,
                                                      jobject platform_params,
                                                      jobject activity);

struct RobloxStartupSymbols {
  JniOnLoadFn jni_on_load = nullptr;
  NativeGameGlobalInitFn game_global_init = nullptr;
  NativeUpdateAdapterInitFn update_adapter_init = nullptr;
  NativeAppBridgeV2InitWithParamsFn app_bridge_v2_init_with_params = nullptr;
  NativeAppBridgeStartLuaAppDmFn app_bridge_start_lua_app_dm = nullptr;
  NativeAppBridgeV2StartAppWithParamsFn app_bridge_v2_start_app_with_params =
      nullptr;
  NativeAppBridgeV2UpdateSurfaceAppFn
      app_bridge_v2_update_surface_app_with_platform_params = nullptr;

  // Optional post-startup capabilities. Callers must use the corresponding
  // flags instead of assuming that a particular Roblox build exports them.
  NativeCallMessagesFromMainThreadFn call_messages_from_main_thread = nullptr;
  NativeAppBridgeV2StartGameWithParamFn app_bridge_v2_start_game_with_param =
      nullptr;
  NativeAppBridgeV2UpdateSurfaceGameFn
      app_bridge_v2_update_surface_game_with_platform_params = nullptr;
};

struct RobloxStartupCapabilityFlags {
  bool has_surface_app = false;
  bool has_message_pump = false;
  bool has_start_game = false;
  bool has_surface_game = false;
};

using NativeGamepadAxisEventFn = void (*)(JNIEnv*, jclass, jint, jint, jfloat,
                                          jfloat, jfloat);
using NativeGamepadButtonEventFn = void (*)(JNIEnv*, jclass, jint, jint, jint);
using NativeGamepadConnectEventWithGamepadTypeFn = void (*)(JNIEnv*, jclass,
                                                            jint, jint);
using NativeGamepadDisconnectEventFn = void (*)(JNIEnv*, jclass, jint);
using NativeGetMainWindowIsMouseLockedCenterFn = jboolean (*)(JNIEnv*, jclass);
using NativePassVector3Fn = void (*)(JNIEnv*, jclass, jfloat, jfloat, jfloat);
using NativePassGyroscopeChangeFn = void (*)(JNIEnv*, jclass, jfloat, jfloat,
                                             jfloat, jfloat, jfloat, jfloat,
                                             jfloat);
using NativePassInputFn = void (*)(JNIEnv*, jclass, jint, jfloat, jfloat, jint,
                                   jint, jint);
using NativePassLongPressGestureFn = void (*)(JNIEnv*, jclass, jint, jfloat,
                                              jfloat);
using NativePassMouseButtonFn = void (*)(JNIEnv*, jclass, jfloat, jfloat,
                                         jboolean, jint);
using NativePassMouseVector4Fn = void (*)(JNIEnv*, jclass, jfloat, jfloat,
                                          jfloat, jfloat);
using NativePassMouseVector3Fn = void (*)(JNIEnv*, jclass, jfloat, jfloat,
                                          jfloat);
using NativePassKeyEventFn = void (*)(JNIEnv*, jclass, jboolean, jint, jint,
                                      jboolean);
using NativeSyncTextboxTextAndCursorPosition2Fn = void (*)(JNIEnv*, jclass,
                                                           jstring, jint);
using NativeGetTextBoxInfoFn = jobject (*)(JNIEnv*, jclass);
using NativePassTextFn = void (*)(JNIEnv*, jclass, jlong, jstring, jboolean,
                                  jint);
using NativeTextboxHandleFn = void (*)(JNIEnv*, jclass, jlong);
using NativePassPanGestureMultitouchFn = void (*)(JNIEnv*, jclass, jint,
                                                  jfloatArray, jfloat, jfloat,
                                                  jfloat, jfloat);
using NativePassGestureWithVelocityFn = void (*)(JNIEnv*, jclass, jint, jfloat,
                                                 jfloat, jfloat, jfloat, jfloat,
                                                 jfloat);
using NativePassTapGestureFn = void (*)(JNIEnv*, jclass, jfloat, jfloat);
using NativeSetSensorEnabledFn = void (*)(JNIEnv*, jclass, jboolean);
using NativeSetGamepadSupportedKeyWithGamepadTypeFn = void (*)(JNIEnv*, jclass,
                                                               jint, jint,
                                                               jboolean, jint);
using NativeSetGamepadSupportedMotionWithGamepadTypeFn =
    void (*)(JNIEnv*, jclass, jint, jint, jint, jboolean, jint);
using NativeSetTvRemoteSupportedKeyFn = void (*)(JNIEnv*, jclass, jint, jint,
                                                 jboolean);
using NativeTvRemoteButtonEventFn = void (*)(JNIEnv*, jclass, jint, jint, jint);
using NativeTvRemoteConnectEventFn = void (*)(JNIEnv*, jclass, jint);
using NativeUpdateScreenOrientationFn = void (*)(JNIEnv*, jclass, jint);

struct RobloxInputSymbols {
  NativeGamepadAxisEventFn gamepad_axis_event = nullptr;
  NativeGamepadButtonEventFn gamepad_button_event = nullptr;
  NativeGamepadConnectEventWithGamepadTypeFn
      gamepad_connect_event_with_gamepad_type = nullptr;
  NativeGamepadDisconnectEventFn gamepad_disconnect_event = nullptr;
  NativeGetMainWindowIsMouseLockedCenterFn
      get_main_window_is_mouse_locked_center = nullptr;
  NativePassVector3Fn pass_accelerometer_change = nullptr;
  NativePassVector3Fn pass_gravity_change = nullptr;
  NativePassGyroscopeChangeFn pass_gyroscope_change = nullptr;
  NativePassInputFn pass_input = nullptr;
  NativePassLongPressGestureFn pass_long_press_gesture = nullptr;
  NativePassMouseButtonFn pass_mouse_button = nullptr;
  NativePassMouseVector4Fn pass_mouse_move = nullptr;
  NativePassMouseVector4Fn pass_mouse_pan = nullptr;
  NativePassMouseVector3Fn pass_mouse_pinch = nullptr;
  NativePassMouseVector3Fn pass_mouse_wheel = nullptr;
  NativePassKeyEventFn pass_key_event = nullptr;
  NativeSyncTextboxTextAndCursorPosition2Fn
      sync_textbox_text_and_cursor_position2 = nullptr;
  NativeGetTextBoxInfoFn get_text_box_info = nullptr;
  NativePassTextFn pass_text = nullptr;
  NativeTextboxHandleFn return_pressed_from_on_screen_keyboard = nullptr;
  NativeTextboxHandleFn release_focus = nullptr;
  NativePassPanGestureMultitouchFn pass_pan_gesture_multitouch = nullptr;
  NativePassGestureWithVelocityFn pass_pan_gesture_with_velocity = nullptr;
  NativePassGestureWithVelocityFn pass_pinch_gesture = nullptr;
  NativePassGestureWithVelocityFn pass_rotate_gesture = nullptr;
  NativePassLongPressGestureFn pass_swipe_gesture = nullptr;
  NativePassTapGestureFn pass_tap_gesture = nullptr;
  NativeSetSensorEnabledFn set_accelerometer_enabled = nullptr;
  NativeSetGamepadSupportedKeyWithGamepadTypeFn
      set_gamepad_supported_key_with_gamepad_type = nullptr;
  NativeSetGamepadSupportedMotionWithGamepadTypeFn
      set_gamepad_supported_motion_with_gamepad_type = nullptr;
  NativeSetSensorEnabledFn set_gyroscope_enabled = nullptr;
  NativeSetTvRemoteSupportedKeyFn set_tv_remote_supported_key = nullptr;
  NativeTvRemoteButtonEventFn tv_remote_button_event = nullptr;
  NativeTvRemoteConnectEventFn tv_remote_connect_event = nullptr;
  NativeTvRemoteConnectEventFn tv_remote_disconnect_event = nullptr;
  NativeUpdateScreenOrientationFn update_screen_orientation = nullptr;
};

enum class RobloxGamepadApi {
  kUnsupported,
  kWithGamepadType,
};

struct RobloxInputCapabilityFlags {
  RobloxGamepadApi gamepad_api = RobloxGamepadApi::kUnsupported;
  bool has_gamepad_events = false;
  bool has_mouse = false;
  bool has_touch = false;
  bool has_hardware_keyboard = false;
  bool has_text_input = false;
  bool has_gestures = false;
  bool has_sensors = false;
  bool has_tv_remote = false;
  bool has_screen_orientation = false;

  // unsupported: these exports were removed from current Roblox
  // builds and must not be selected as a fallback for the modern API.
  bool has_legacy_gamepad_api = false;
  bool has_ptf_gamepad_api = false;
};

struct RobloxCapabilities {
  RobloxStartupSymbols startup;
  RobloxStartupCapabilityFlags startup_flags;
  RobloxInputSymbols input;
  RobloxInputCapabilityFlags input_flags;
};

enum class RobloxCapabilityResolutionStatus {
  kReady,
  kInvalidLookup,
  kMissingRequiredSymbols,
};

// A failed resolution withholds the partially resolved function
// table. This prevents callers from starting Roblox after a required export was
// not found, while still exposing exact missing symbol names for diagnostics.
class RobloxCapabilityResolution {
 public:
  RobloxCapabilityResolutionStatus status() const { return status_; }
  bool ok() const {
    return status_ == RobloxCapabilityResolutionStatus::kReady;
  }
  const RobloxCapabilities* capabilities() const {
    return ok() ? &capabilities_ : nullptr;
  }
  const std::vector<std::string>& missing_required_symbols() const {
    return missing_required_symbols_;
  }

 private:
  friend RobloxCapabilityResolution ResolveRobloxCapabilities(
      const RobloxSymbolLookup& lookup,
      RobloxCapabilityRequirements requirements);

  RobloxCapabilityResolutionStatus status_ =
      RobloxCapabilityResolutionStatus::kInvalidLookup;
  RobloxCapabilities capabilities_;
  std::vector<std::string> missing_required_symbols_;
};

RobloxCapabilityResolution ResolveRobloxCapabilities(
    const RobloxSymbolLookup& lookup,
    RobloxCapabilityRequirements requirements = {});

const char* RobloxCapabilityResolutionStatusName(
    RobloxCapabilityResolutionStatus status);
const char* RobloxGamepadApiName(RobloxGamepadApi api);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_ROBLOX_CAPABILITY_RESOLVER_H_
