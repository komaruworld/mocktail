#ifndef MOCKTAIL_RUNTIME_ROBLOX_FRESH_GAME_LAUNCH_CONTROLLER_H_
#define MOCKTAIL_RUNTIME_ROBLOX_FRESH_GAME_LAUNCH_CONTROLLER_H_

#include <jni.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "mocktail/status.h"
#include "runtime/roblox_experience_launch_bridge.h"
#include "runtime/roblox_game_session_native_adapter.h"

namespace mocktail {
namespace runtime {

// Borrowed LuaApp JNI objects plus owned identity/surface values needed
// to create one fresh UGCGame session. All borrowed JNI references need only
// remain valid for the synchronous Launch call; RobloxGameSessionRuntime
// promotes the complete binding to global references before native start.
struct RobloxFreshGameLaunchContext {
  uint64_t request_id = 0;
  jclass native_gl_class = nullptr;
  jobject surface = nullptr;
  jobject platform_params = nullptr;
  jobject activity = nullptr;
  jobject device_params = nullptr;
  jobject vr_context = nullptr;
  GameSessionPrincipal principal;
  GameSurface game_surface;
  std::string username;
  bool is_under_13 = false;
  int32_t join_request_type = -1;

  bool complete() const;
};

using FreshLaunchPresentObserver = void (*)(void* context,
                                            uint64_t frame_serial);
using RegisterFreshLaunchPresentObserverFn = bool (*)(
    void* context, FreshLaunchPresentObserver observer, void* observer_context);
using ClearFreshLaunchPresentObserverFn = void (*)(void* context);

// Production composition adapts this to window::ScopedPresentObserver.
// clear_observer must wait for an in-flight callback before returning.
// Boundary callbacks must not re-enter this controller.
struct RobloxFreshLaunchPresentBoundary {
  void* context = nullptr;
  RegisterFreshLaunchPresentObserverFn register_observer = nullptr;
  ClearFreshLaunchPresentObserverFn clear_observer = nullptr;

  bool complete() const {
    return register_observer != nullptr && clear_observer != nullptr;
  }
};

// Builds a new local StartGameParams JNI object from the request. No field is
// read from environment variables or from a previous game session. Caller
// owns the returned local reference.
Status BuildFreshStartGameParams(JNIEnv* env,
                                 const RobloxExperienceLaunchRequest& request,
                                 const RobloxFreshGameLaunchContext& context,
                                 jobject* result);

// Owns one LuaApp -> UGCGame transition. Observer registration precedes native
// start so a host present emitted during start is retained. Leave returns to
// LuaApp and releases only per-session JNI references, allowing another fresh
// launch. Shutdown is terminal and destroys an active app-level native runtime.
class RobloxFreshGameLaunchController final {
 public:
  RobloxFreshGameLaunchController(
      JniEnvironmentProvider environment, RobloxGameSessionSymbols symbols,
      RobloxFreshLaunchPresentBoundary present_boundary,
      RobloxGameSurfaceJniConfig surface_config = {},
      RobloxGamePresentedObserver presented_observer = {});
  ~RobloxFreshGameLaunchController();

  RobloxFreshGameLaunchController(const RobloxFreshGameLaunchController&) =
      delete;
  RobloxFreshGameLaunchController& operator=(
      const RobloxFreshGameLaunchController&) = delete;

  Status Launch(const RobloxExperienceLaunchRequest& request,
                const RobloxFreshGameLaunchContext& context);
  Status Leave();
  // Records an already-completed native UGCGame -> LuaApp transition. Unlike
  // Leave(), this must not invoke native lifecycle callbacks because Roblox
  // has performed them before publishing the LuaApp stage observation.
  Status ObserveLuaAppReturn();
  Status Shutdown();
  GameSessionUpdateResult SurfaceCreated(uint64_t surface_generation);
  GameSessionUpdateResult SurfaceChanged(GameSurface surface);
  GameSessionUpdateResult RebindSurface(GameSurface surface);
  GameSessionUpdateResult SurfaceDestroyed(uint64_t surface_generation);
  bool active() const;
  GameSessionSnapshot Snapshot() const;

 private:
  const JniEnvironmentProvider environment_;
  const RobloxGameSessionSymbols symbols_;
  const RobloxFreshLaunchPresentBoundary present_boundary_;
  const RobloxGameSurfaceJniConfig surface_config_;
  const RobloxGamePresentedObserver presented_observer_;

  mutable std::mutex mutex_;
  std::unique_ptr<RobloxGameSessionRuntime> runtime_;
  GameSessionSnapshot last_snapshot_;
  bool observer_registered_ = false;
  bool active_ = false;
  bool app_shutdown_ = false;
};

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_ROBLOX_FRESH_GAME_LAUNCH_CONTROLLER_H_
