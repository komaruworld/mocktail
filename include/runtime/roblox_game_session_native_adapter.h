#ifndef MOCKTAIL_RUNTIME_ROBLOX_GAME_SESSION_NATIVE_ADAPTER_H_
#define MOCKTAIL_RUNTIME_ROBLOX_GAME_SESSION_NATIVE_ADAPTER_H_

#include <jni.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "mocktail/status.h"
#include "runtime/game_session_coordinator.h"
#include "runtime/roblox_app_lifecycle.h"
#include "runtime/roblox_capability_resolver.h"

namespace mocktail {
namespace runtime {

using NativeAppBridgeV2PauseGameFn = void (*)(JNIEnv* env, jclass clazz);
using NativeAppBridgeV2ResumeGameFn = void (*)(JNIEnv* env, jclass clazz,
                                               jobject surface,
                                               jobject platform_params,
                                               jobject activity);
using NativeAppBridgeV2LeaveGameFn = void (*)(JNIEnv* env, jclass clazz);

struct RobloxGameSessionSymbols {
  SetTaskSchedulerBackgroundModeFn set_task_scheduler_background_mode = nullptr;
  NativeAppBridgeV2StartGameWithParamFn start_game = nullptr;
  NativeAppBridgeV2UpdateSurfaceGameFn update_surface = nullptr;
  NativeAppBridgeV2PauseGameFn pause_game = nullptr;
  NativeAppBridgeV2ResumeGameFn resume_game = nullptr;
  NativeAppBridgeV2LeaveGameFn leave_game = nullptr;
  NativeAppBridgeV2PauseAppFn pause_app = nullptr;
  NativeAppBridgeV2DestroyAppFn destroy_app = nullptr;
  NativeAppBridgeV2UpdateSurfaceAppFn update_surface_app = nullptr;
  NativeAppBridgeV2StartAppWithParamsFn start_app = nullptr;
  NativeCallMessagesFromMainThreadFn call_messages_from_main_thread = nullptr;

  bool complete() const;
  RobloxAppLifecycleSymbols app_lifecycle_symbols() const;
};

enum class RobloxGameSessionResolutionStatus {
  kReady,
  kInvalidLookup,
  kMissingRequiredSymbols,
};

class RobloxGameSessionResolution final {
 public:
  RobloxGameSessionResolutionStatus status() const { return status_; }
  bool ok() const {
    return status_ == RobloxGameSessionResolutionStatus::kReady;
  }
  const RobloxGameSessionSymbols* symbols() const {
    return ok() ? &symbols_ : nullptr;
  }
  const std::vector<std::string>& missing_required_symbols() const {
    return missing_required_symbols_;
  }

 private:
  friend RobloxGameSessionResolution ResolveRobloxGameSessionSymbols(
      const RobloxSymbolLookup& lookup);

  RobloxGameSessionResolutionStatus status_ =
      RobloxGameSessionResolutionStatus::kInvalidLookup;
  RobloxGameSessionSymbols symbols_;
  std::vector<std::string> missing_required_symbols_;
};

RobloxGameSessionResolution ResolveRobloxGameSessionSymbols(
    const RobloxSymbolLookup& lookup);

using PrepareJniEnvironmentFn = void (*)(void* context);

// The provider owns JavaVM lifetime and optional pseudo-VM function-table
// restoration. Acquire attaches the current thread when necessary and never
// retains the returned JNIEnv beyond the synchronous native operation.
struct JniEnvironmentProvider {
  JavaVM* java_vm = nullptr;
  void* prepare_context = nullptr;
  PrepareJniEnvironmentFn prepare = nullptr;

  bool valid() const { return java_vm != nullptr; }
  Status Acquire(JNIEnv** env) const;
};

struct RobloxGameSessionJniObjects {
  jclass native_gl_class = nullptr;
  jobject surface = nullptr;
  jobject platform_params = nullptr;
  jobject activity = nullptr;
  jobject start_game_params = nullptr;

  // Activity is optional in the APK ABI and may remain null.
  bool complete() const {
    return native_gl_class != nullptr && surface != nullptr &&
           platform_params != nullptr && start_game_params != nullptr;
  }
};

// Values copied from the Android PlatformParams contract used by the current
// APK. Surface dimensions are intentionally not stored here: the guest queries
// them from ANativeWindow, while PlatformParams carries only device/input and
// physical viewport metadata.
struct RobloxGameSurfaceJniConfig {
  std::string asset_folder_path = "rbx_bin/assets";
  float dpi_scale = 1.0f;
  int32_t viewport_width_mm = 203;
  int32_t viewport_height_mm = 114;
  bool is_touch_device = false;
  bool is_mouse_device = true;
  bool is_keyboard_device = true;

  bool complete() const {
    return !asset_folder_path.empty() && dpi_scale > 0.0f &&
           viewport_width_mm > 0 && viewport_height_mm > 0;
  }
};

// Associates a prebuilt StartGameParams object with the request that
// produced it. A mismatched invocation is rejected instead of starting with
// stale launch data.
struct RobloxGameSessionBinding {
  RobloxGameSessionJniObjects objects;
  GameSessionPrincipal principal;
  GameJoinRequest request;
  GameSurface surface;

  bool complete() const {
    const bool valid_principal =
        principal.generation != 0 && !principal.base_url.empty() &&
        ((principal.kind == GameSessionPrincipalKind::kAuthenticated &&
          !principal.principal_id.empty()) ||
         (principal.kind == GameSessionPrincipalKind::kLocalGuest &&
          principal.principal_id.empty()));
    const bool valid_request = request.request_id != 0 &&
                               request.place_id >= 0 &&
                               !request.launch_parameters.empty();
    const bool valid_surface = surface.generation != 0 &&
                               surface.native_window != 0 &&
                               surface.width != 0 && surface.height != 0;
    return objects.complete() && valid_principal && valid_request &&
           valid_surface;
  }
};

// JNI implementation of GameSessionCapabilities. It owns only JNI global
// references and function pointers; JavaVM, loader and WSI lifetimes
// remain composition-owned and must outlive this object.
class RobloxGameSessionNativeAdapter final {
 public:
  RobloxGameSessionNativeAdapter(
      JniEnvironmentProvider environment, RobloxGameSessionSymbols symbols,
      RobloxGameSurfaceJniConfig surface_config = {});
  ~RobloxGameSessionNativeAdapter();

  RobloxGameSessionNativeAdapter(const RobloxGameSessionNativeAdapter&) =
      delete;
  RobloxGameSessionNativeAdapter& operator=(
      const RobloxGameSessionNativeAdapter&) = delete;

  Status Initialize(const RobloxGameSessionBinding& binding);
  Status Release();
  bool initialized() const;
  GameSessionCapabilities Capabilities();

 private:
  static Status SetForegroundCallback(void* context);
  static Status StartCallback(void* context,
                              const GameSessionPrincipal& principal,
                              const GameJoinRequest& request,
                              const GameSurface& surface);
  static Status UpdateCallback(void* context, const GameSurface& surface);
  static Status PauseCallback(void* context);
  static Status ResumeCallback(void* context, const GameSurface& surface);
  static Status LeaveCallback(void* context);
  static Status ShutdownCallback(void* context);

  Status SetForeground();
  Status Start(const GameSessionPrincipal& principal,
               const GameJoinRequest& request, const GameSurface& surface);
  Status Update(const GameSurface& surface);
  Status Pause();
  Status Resume(const GameSurface& surface);
  Status Leave();
  Status Shutdown();
  Status InvokeWithMainThreadPump(NativeAppBridgeV2PauseGameFn operation,
                                  const char* operation_name);
  Status RebindAndInvoke(const GameSurface& surface, bool resume);
  Status CreateSurfaceObjectLocked(JNIEnv* env, const GameSurface& surface,
                                   jobject* result) const;
  Status CreatePlatformParamsLocked(JNIEnv* env, jobject* result) const;
  Status CheckJniException(JNIEnv* env, const char* operation) const;
  void ReleaseGlobalReferenceLocked(JNIEnv* env, jobject reference);
  void ReleaseGlobalReferencesLocked(JNIEnv* env);

  const JniEnvironmentProvider environment_;
  const RobloxGameSessionSymbols symbols_;
  const RobloxGameSurfaceJniConfig surface_config_;
  RobloxAppLifecycle app_lifecycle_;
  mutable std::mutex callback_mutex_;
  RobloxGameSessionBinding binding_;
  GameSurface retained_surface_;
  bool initialized_ = false;
};

using RobloxGamePresentedCallback = void (*)(void* context,
                                             uint64_t frame_serial);

struct RobloxGamePresentedObserver {
  void* context = nullptr;
  RobloxGamePresentedCallback notify = nullptr;

  bool valid() const { return notify != nullptr; }
};

// Small production owner that binds the JNI adapter to the policy coordinator.
// Initial startup always follows the APK sequence:
// principal -> join request -> SurfaceCreated(foreground) ->
// SurfaceChanged(Start).
class RobloxGameSessionRuntime final {
 public:
  RobloxGameSessionRuntime(JniEnvironmentProvider environment,
                           RobloxGameSessionSymbols symbols,
                           RobloxGameSurfaceJniConfig surface_config = {},
                           RobloxGamePresentedObserver presented_observer = {});

  RobloxGameSessionRuntime(const RobloxGameSessionRuntime&) = delete;
  RobloxGameSessionRuntime& operator=(const RobloxGameSessionRuntime&) = delete;

  Status InitializeAndStart(const RobloxGameSessionBinding& binding,
                            GameSessionPrincipal principal,
                            GameJoinRequest request, GameSurface surface);
  GameSessionUpdateResult SurfaceCreated(uint64_t surface_generation);
  GameSessionUpdateResult SurfaceChanged(GameSurface surface);
  GameSessionUpdateResult RebindSurface(GameSurface surface);
  GameSessionUpdateResult SurfaceDestroyed(uint64_t surface_generation);
  GameSessionUpdateResult RecordPresent(uint64_t frame_serial);
  GameSessionUpdateResult LeaveGame();
  // Process-level teardown pauses the active surface and returns UGCGame to
  // the app before destroying its runtime. Keeping this orchestration above
  // the coordinator preserves the explicit SurfaceDestroyed/LeaveGame/
  // Shutdown distinction.
  GameSessionUpdateResult Shutdown();
  GameSessionSnapshot Snapshot() const;
  Status startup_status() const;

  static void SuccessfulPresentCallback(void* context, uint64_t frame_serial);

 private:
  RobloxGameSessionNativeAdapter adapter_;
  GameSessionCoordinator coordinator_;
  const RobloxGamePresentedObserver presented_observer_;
  // Makes the process-level pause/leave/destroy sequence atomic with respect
  // to surface updates. Present evidence bypasses this mutex
  // because StartGame can report it synchronously from the render callback.
  mutable std::mutex lifecycle_mutex_;
  // The coordinator accepts only the first successful game present. Keep the
  // production observer's steady-state path lock-free after that evidence has
  // been recorded; resize readiness owns its separate per-generation gate.
  std::atomic<bool> present_evidence_recorded_{false};
  mutable std::mutex status_mutex_;
  Status startup_status_ = Status::Error(StatusCode::kFailedPrecondition,
                                         "game session has not started");
};

const char* RobloxGameSessionResolutionStatusName(
    RobloxGameSessionResolutionStatus status);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_ROBLOX_GAME_SESSION_NATIVE_ADAPTER_H_
