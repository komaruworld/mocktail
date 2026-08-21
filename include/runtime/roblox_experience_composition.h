#ifndef MOCKTAIL_RUNTIME_ROBLOX_EXPERIENCE_COMPOSITION_H_
#define MOCKTAIL_RUNTIME_ROBLOX_EXPERIENCE_COMPOSITION_H_

#include <jni.h>

#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include "mocktail/status.h"
#include "runtime/owned_pthread.h"
#include "runtime/roblox_browser_service_bridge.h"
#include "runtime/roblox_experience_launch_bridge.h"
#include "runtime/roblox_experience_presence.h"
#include "runtime/roblox_fresh_game_launch_controller.h"
#include "runtime/roblox_web_view_bridge.h"
#include "runtime/webview_roblox_cookie.h"

namespace jnivm {
class VM;
}  // namespace jnivm

namespace mocktail {
namespace runtime {

class RobloxExperienceCompositionWebSurfaceTest;

// Pseudo-JVM integration needed to materialize the APK RawCallback interface.
// The composition obtains MessageBus.f() itself through its exact Java API.
struct RobloxExperienceJniFactory {
  void* context = nullptr;
  jobject (*create_raw_callback)(void* context,
                                 std::shared_ptr<void> callback_context,
                                 void (*run)(void*, JNIEnv*,
                                             jstring)) = nullptr;
  void (*clear_raw_callback)(void* context, jobject callback) = nullptr;
  jobject (*create_request_handler)(void* context,
                                    std::shared_ptr<void> callback_context,
                                    std::string (*run)(void*, JNIEnv*,
                                                       jstring)) = nullptr;
  void (*clear_request_handler)(void* context, jobject handler) = nullptr;
  jobject (*create_mem_storage_callback)(
      void* context, std::shared_ptr<void> callback_context,
      void (*on_item_set)(void*, JNIEnv*, jstring)) = nullptr;
  void (*clear_mem_storage_callback)(void* context, jobject callback) = nullptr;
  bool (*set_platform_web_callbacks)(
      void* context, std::shared_ptr<void> callback_context,
      void (*on_data_model_notification)(void*, JNIEnv*, jstring, jstring),
      void (*on_app_bridge_notification)(void*, JNIEnv*, jstring, jstring),
      void (*on_native_overlay)(void*, JNIEnv*, jstring, jstring),
      void (*on_open_web_activity)(void*, JNIEnv*, jstring, jstring),
      void (*on_sync_cookies)(void*, JNIEnv*, jstring),
      void (*on_set_cookie)(void*, JNIEnv*, jstring, jstring)) = nullptr;
  void (*clear_platform_web_callbacks)(void* context) = nullptr;

  bool complete() const {
    return context != nullptr && create_raw_callback != nullptr &&
           clear_raw_callback != nullptr && create_request_handler != nullptr &&
           clear_request_handler != nullptr &&
           create_mem_storage_callback != nullptr &&
           clear_mem_storage_callback != nullptr &&
           set_platform_web_callbacks != nullptr &&
           clear_platform_web_callbacks != nullptr;
  }
};

// Immutable values captured when LuaApp becomes ready. No Stage 6 local JNI
// reference is accepted here: the owner builds a fresh object graph and keeps
// its roots as global references until process-level shutdown.
struct RobloxLuaAppExperienceReadiness {
  GameSessionPrincipal principal;
  GameSurface surface;
  std::string username;
  bool is_under_13 = false;
  int32_t join_request_type = -1;

  bool complete() const;
};

// Supplies the current host surface to a composition that becomes
// authenticated after LuaApp startup. Production defaults to the SDL window
// snapshot; tests may inject an equivalent source.
struct RobloxExperienceSurfaceProvider {
  void* context = nullptr;
  GameSurface (*snapshot)(void* context) = nullptr;

  bool valid() const { return snapshot != nullptr; }
};

// Production composition for the APK platform protocols and ExperienceProtocol
// paths. InitializePlatformProtocols retains MessageBus.f(), installs the
// WebViewProtocol, and binds the durable BrowserService MemStorage observers
// before native bootstrap. OnLuaAppReady then adds the game JNI roots and
// ExperienceProtocol subscription. Every owned JSON launch request is
// dispatched to a fresh UGCGame controller without environment-derived fields.
class RobloxExperienceComposition final {
 public:
  RobloxExperienceComposition(
      JniEnvironmentProvider environment,
      RobloxExperienceMessageBusSymbols message_bus_symbols,
      RobloxWebViewMessageBusSymbols web_view_symbols,
      RobloxBrowserServiceSymbols browser_service_symbols,
      RobloxGameSessionSymbols game_symbols,
      RobloxExperienceJniFactory jni_factory,
      RobloxFreshLaunchPresentBoundary present_boundary,
      RobloxGameSurfaceJniConfig surface_config = {},
      const SecureRobloxCredential* initial_web_view_credential = nullptr,
      RobloxExperienceSurfaceProvider surface_provider = {},
      RobloxExperiencePresenceObserver presence_observer = {},
      bool clear_persisted_web_view_cookie = false);
  ~RobloxExperienceComposition();

  RobloxExperienceComposition(const RobloxExperienceComposition&) = delete;
  RobloxExperienceComposition& operator=(const RobloxExperienceComposition&) =
      delete;

  Status InitializePlatformProtocols();
  Status OnLuaAppReady(RobloxLuaAppExperienceReadiness readiness);
  // Callback-safe notification from the native app lifecycle. The callback
  // never invokes JNI or waits for an in-flight launch worker.
  void NotifyLuaAppDidReturn();
  Status DrainPlatformEvents();
  Status DrainLaunchRequests();
  Status LeaveGame();
  GameSessionUpdateResult SurfaceCreated(uint64_t generation);
  GameSessionUpdateResult SurfaceChanged(GameSurface surface);
  GameSessionUpdateResult SurfaceDestroyed(uint64_t generation);
  Status Shutdown();

  bool subscribed() const;
  GameSessionSnapshot Snapshot() const;

 private:
  friend class RobloxExperienceCompositionWebSurfaceTest;
  struct GlobalObjects;
  struct LaunchTask;
  struct WebSurfaceExitTarget;
  struct WebSurfaceExitContext;
  struct LifecycleTarget;

  enum class WebSurfaceRoute {
    kNone,
    kWebView,
    kBrowserService,
  };

  struct WebSurfacePresentation {
    std::string title = "Roblox";
    bool visible = true;
    bool back_navigation_disabled = false;
    bool show_domain_as_title = false;
  };

  static Status DispatchLaunch(void* context,
                               const RobloxExperienceLaunchRequest& request);
  Status DrainExternalLaunchRequests();
  static Status DispatchWebViewOpen(void* context,
                                    const RobloxWebViewOpenRequest& request,
                                    WebViewHelperExitObserver exit_observer);
  static Status DispatchWebViewMutate(
      void* context, const RobloxWebViewMutationRequest& request);
  static Status DispatchWebViewClose(void* context);
  static Status DispatchWebViewCookie(void* context,
                                      std::string_view canonical_header);
  static Status DispatchBrowserServiceOpen(
      void* context, const RobloxBrowserServiceOpenRequest& request,
      WebViewHelperExitObserver exit_observer);
  static Status DispatchBrowserServiceClose(void* context);
  static Status DispatchBrowserServiceConfig(
      void* context, const RobloxBrowserServiceConfigRequest& request);
  static Status DispatchBrowserServiceExecute(
      void* context, const RobloxBrowserServiceExecuteRequest& request);
  static void WebSurfaceExited(void* context);
  static void NotifyLateLuaAppDidReturn(void* context);
  static void GamePresented(void* context, uint64_t frame_serial);
  static void* RunLaunchWorker(void* context);
  static Status RouteWebSurfaceEvent(WebSurfaceRoute route,
                                     const WebViewHelperEvent& event,
                                     RobloxWebViewBridge* web_view_bridge);
  Status RouteCurrentWebSurfaceEvent(
      const std::shared_ptr<WebViewHelperProcess>& source_process,
      uint64_t process_generation, uint64_t logical_generation,
      const WebViewHelperEvent& event);
  Status OpenWebSurface(const std::string& url, const char* transport,
                        WebSurfaceRoute route,
                        WebViewHelperExitObserver exit_observer,
                        const WebSurfacePresentation& presentation);
  // WebViewProtocol and BrowserService are two logical transports over the
  // APK's single physical WebView container. Either protocol may close or
  // mutate the surface that the other one opened.
  Status CloseWebSurface();
  void HandleWebSurfaceExit(uint64_t process_generation);
  Status Dispatch(const RobloxExperienceLaunchRequest& request);
  Status PromoteAuthenticatedSession();
  Status RefreshLateSurface();
  void PublishPresentedPresence(uint64_t frame_serial);
  void RunActiveLaunch();
  Status BuildPlatformGlobalObjects();
  Status BuildLuaAppGlobalObjects();
  Status BuildLaunchObjects(const GameSurface& surface, jobject* surface_object,
                            jobject* platform_params);
  Status BuildLuaAppStartParams(
      JNIEnv* env, const RobloxLuaAppExperienceReadiness& readiness,
      jobject surface_object, jobject platform_params, jobject activity,
      jobject* start_app_params);
  Status RebindLuaAppSurface(const GameSurface& surface);
  Status RestartLuaAppSurface(const GameSurface& surface);
  Status ReleaseLuaAppGlobalObjects();
  Status ReleaseGlobalObjects();
  void NotifyPresence(RobloxExperiencePresencePhase phase,
                      const RobloxExperienceLaunchRequest* request) const;

  const JniEnvironmentProvider environment_;
  const RobloxExperienceMessageBusSymbols message_bus_symbols_;
  const RobloxWebViewMessageBusSymbols web_view_symbols_;
  const RobloxBrowserServiceSymbols browser_service_symbols_;
  const RobloxGameSessionSymbols game_symbols_;
  const RobloxExperienceJniFactory jni_factory_;
  const RobloxFreshLaunchPresentBoundary present_boundary_;
  const RobloxGameSurfaceJniConfig surface_config_;
  const bool consumes_window_surface_events_;
  const RobloxExperienceSurfaceProvider surface_provider_;
  const RobloxExperiencePresenceObserver presence_observer_;

  mutable std::mutex mutex_;
  std::mutex surface_operation_mutex_;
  std::mutex web_surface_operation_mutex_;
  RobloxLuaAppExperienceReadiness readiness_;
  std::unique_ptr<GlobalObjects> objects_;
  std::shared_ptr<RobloxFreshGameLaunchController> controller_;
  std::unique_ptr<RobloxExperienceLaunchBridge> bridge_;
  std::unique_ptr<RobloxWebViewBridge> web_view_bridge_;
  std::unique_ptr<RobloxBrowserServiceBridge> browser_service_bridge_;
  std::shared_ptr<WebViewHelperProcess> web_surface_process_;
  std::shared_ptr<WebSurfaceExitTarget> web_surface_exit_target_;
  std::shared_ptr<LifecycleTarget> lifecycle_target_;
  WebViewHelperExitObserver web_surface_logical_exit_observer_;
  WebSurfaceRoute web_surface_route_ = WebSurfaceRoute::kNone;
  uint64_t web_surface_process_generation_ = 0;
  uint64_t web_surface_logical_generation_ = 0;
  uint64_t next_web_surface_process_generation_ = 1;
  uint64_t next_web_surface_logical_generation_ = 1;
  SecureWebViewRobloxCookie web_view_cookie_;
  std::string web_view_cookie_initialization_error_;
  bool web_view_cookie_synchronized_ = false;
  bool clear_persisted_web_view_cookie_ = false;
  std::deque<RobloxExperienceLaunchRequest> pending_launch_requests_;
  std::unique_ptr<LaunchTask> active_launch_;
  std::optional<RobloxExperienceLaunchRequest> presence_request_;
  std::string active_game_canonical_json_;
  OwnedPthread launch_worker_;
  uint64_t next_request_id_ = 1;
  bool platform_protocols_initialized_ = false;
  bool subscribed_ = false;
  bool launch_in_progress_ = false;
  bool game_active_ = false;
  bool playing_presence_published_ = false;
  bool lua_app_return_pending_ = false;
  bool controlled_switch_waiting_for_return_ = false;
  bool lua_app_surface_recreation_pending_ = false;
  bool late_surface_tracking_ = false;
  GameSurface late_surface_snapshot_;
  jnivm::VM* late_lifecycle_vm_ = nullptr;
};

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_ROBLOX_EXPERIENCE_COMPOSITION_H_
