#ifndef MOCKTAIL_RUNTIME_ROBLOX_BROWSER_SERVICE_BRIDGE_H_
#define MOCKTAIL_RUNTIME_ROBLOX_BROWSER_SERVICE_BRIDGE_H_

#include <jni.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>

#include "mocktail/status.h"
#include "runtime/roblox_game_session_native_adapter.h"
#include "runtime/webview_helper_launcher.h"

namespace mocktail {
namespace runtime {

inline constexpr char kRobloxBrowserServiceOpenWindowKey[] =
    "BrowserService.OpenBrowserWindow";
inline constexpr char kRobloxBrowserServiceCloseWindowKey[] =
    "BrowserService.CloseBrowserWindow";
inline constexpr char kRobloxBrowserServiceSendCommandKey[] =
    "BrowserService.SendCommand";
inline constexpr char kRobloxBrowserServiceExecuteJavaScriptKey[] =
    "BrowserService.ExecuteJavaScript";
inline constexpr char kRobloxBrowserServiceJavaScriptCallbackKey[] =
    "BrowserService.JavaScriptCallback";
inline constexpr char kRobloxBrowserServiceBrowserWindowClosedKey[] =
    "BrowserService.BrowserWindowClosed";
inline constexpr std::size_t kMaximumRobloxBrowserServicePayloadBytes =
    64 * 1024;
inline constexpr std::size_t kMaximumRobloxBrowserServiceUrlBytes = 16 * 1024;
inline constexpr std::size_t kMaximumRobloxBrowserServiceQueuedEvents = 64;
inline constexpr std::size_t kMaximumRobloxBrowserServiceQueuedBytes =
    1024 * 1024;

// Owned browser request copied before Callback.onItemSet(String) returns.
// URL policy and navigation remain the responsibility of the downstream
// launcher; this boundary validates only the APK contract and bounded input.
// Raw OpenBrowserWindow leaves every presentation property absent, while the
// SendCommand open form engages each property with the APK's default value.
struct RobloxBrowserServiceOpenRequest {
  std::string url;
  std::optional<std::string> title;
  std::optional<bool> visible;
  std::optional<bool> modal;
  std::optional<bool> back_navigation_disabled;
  std::optional<bool> show_domain_as_title;
};

// BrowserService.SendCommand's config form preserves field absence. The APK
// changes only properties present in the JSON object.
struct RobloxBrowserServiceConfigRequest {
  std::optional<std::string> title;
  std::optional<bool> visible;
  std::optional<bool> back_navigation_disabled;
};

struct RobloxBrowserServiceExecuteRequest {
  std::string source;
};

using RobloxBrowserServiceCommand =
    std::variant<RobloxBrowserServiceOpenRequest,
                 RobloxBrowserServiceConfigRequest>;

Status ParseRobloxBrowserServiceOpenWindow(
    const std::string& payload, RobloxBrowserServiceOpenRequest* request);
Status ParseRobloxBrowserServiceSendCommand(
    const std::string& json, RobloxBrowserServiceCommand* command);

using DispatchRobloxBrowserServiceOpenFn =
    Status (*)(void* context, const RobloxBrowserServiceOpenRequest& request,
               WebViewHelperExitObserver exit_observer);
using DispatchRobloxBrowserServiceConfigFn =
    Status (*)(void* context, const RobloxBrowserServiceConfigRequest& request);
using DispatchRobloxBrowserServiceCloseFn = Status (*)(void *context);
using DispatchRobloxBrowserServiceExecuteFn = Status (*)(
    void* context, const RobloxBrowserServiceExecuteRequest& request);

struct RobloxBrowserServiceSink {
  void *context = nullptr;
  DispatchRobloxBrowserServiceOpenFn dispatch_open = nullptr;
  DispatchRobloxBrowserServiceCloseFn dispatch_close = nullptr;
  DispatchRobloxBrowserServiceConfigFn dispatch_config = nullptr;
  DispatchRobloxBrowserServiceExecuteFn dispatch_execute = nullptr;

  bool valid() const {
    return dispatch_open != nullptr && dispatch_close != nullptr &&
           dispatch_config != nullptr && dispatch_execute != nullptr;
  }
};

using BindRobloxMemStorageFn = jobject (*)(JNIEnv *, jclass, jstring, jobject);
using DisconnectRobloxMemStorageFn = void (*)(JNIEnv *, jclass, jobject);
using ReleaseRobloxMemStorageConnectionFn = void (*)(JNIEnv *, jclass, jobject);
using FireRobloxMemStorageFn = void (*)(JNIEnv*, jclass, jstring, jstring);

struct RobloxBrowserServiceSymbols {
  BindRobloxMemStorageFn bind = nullptr;
  DisconnectRobloxMemStorageFn disconnect = nullptr;
  ReleaseRobloxMemStorageConnectionFn release_connection = nullptr;
  FireRobloxMemStorageFn fire = nullptr;

  bool complete() const {
    return bind != nullptr && disconnect != nullptr &&
           release_connection != nullptr && fire != nullptr;
  }
};

struct RobloxBrowserServiceCallbackFactory {
  void *context = nullptr;
  jobject (*create_callback)(void *context,
                             std::shared_ptr<void> callback_context,
                             void (*on_item_set)(void *, JNIEnv *,
                                                 jstring)) = nullptr;
  void (*clear_callback)(void *context, jobject callback) = nullptr;

  bool complete() const {
    return context != nullptr && create_callback != nullptr &&
           clear_callback != nullptr;
  }
};

// Host counterpart to the APK's yg.c and gi.a BrowserService bridges. It binds
// the four expected MemStorage keys. Shutdown disconnects before releasing its
// callbacks and connections. The platform main thread drains outgoing WebView
// events without holding JNI references.
class RobloxBrowserServiceBridge final {
public:
  RobloxBrowserServiceBridge(JniEnvironmentProvider environment,
                             RobloxBrowserServiceSymbols symbols,
                             RobloxBrowserServiceCallbackFactory factory,
                             RobloxBrowserServiceSink sink);
  ~RobloxBrowserServiceBridge();

  RobloxBrowserServiceBridge(const RobloxBrowserServiceBridge &) = delete;
  RobloxBrowserServiceBridge& operator=(const RobloxBrowserServiceBridge&) =
      delete;

  Status Initialize();
  Status Shutdown();

  Status HandleOwnedOpenWindow(std::string payload);
  Status HandleOwnedSendCommand(std::string payload);
  Status HandleOwnedExecuteJavaScript(std::string payload);
  Status HandleCloseWindow();

  Status QueueJavaScriptCallback(std::string payload);
  Status QueueBrowserWindowClosed();
  Status DrainOutgoingEvents();

  bool initialized() const;

private:
 enum class EventKind {
   kOpenWindow,
   kCloseWindow,
   kSendCommand,
   kExecuteJavaScript,
 };

 enum class OutgoingEventKind {
   kJavaScriptCallback,
   kBrowserWindowClosed,
 };

 struct OutgoingEvent {
   OutgoingEventKind kind = OutgoingEventKind::kJavaScriptCallback;
   std::string payload;
 };

  struct CallbackTarget;
  struct BrowserWindowCloseTarget;
  struct BrowserWindowExitContext;

  Status CheckJniException(JNIEnv *env, const char *operation) const;
  Status BeginDispatch();
  void EndDispatch();
  Status DispatchOpenRequest(RobloxBrowserServiceOpenRequest request);
  Status QueueOutgoingEvent(OutgoingEventKind kind, std::string payload);
  void FinishOutgoingDrain(std::deque<OutgoingEvent> remaining_events);
  Status HandleItemSet(EventKind kind, JNIEnv *env, jstring payload);
  static void ItemSetCallback(void *context, JNIEnv *env, jstring payload);
  static void BrowserWindowExited(void* context);

  const JniEnvironmentProvider environment_;
  const RobloxBrowserServiceSymbols symbols_;
  const RobloxBrowserServiceCallbackFactory factory_;
  const RobloxBrowserServiceSink sink_;

  mutable std::mutex mutex_;
  std::condition_variable dispatch_drained_;
  jclass connection_class_ = nullptr;
  std::array<jobject, 4> callbacks_{};
  std::array<jobject, 4> connections_{};
  std::array<std::shared_ptr<CallbackTarget>, 4> callback_targets_{};
  std::shared_ptr<BrowserWindowCloseTarget> browser_window_close_target_;
  std::deque<OutgoingEvent> outgoing_events_;
  std::size_t outgoing_event_bytes_ = 0;
  std::size_t outgoing_drain_events_ = 0;
  std::size_t outgoing_drain_bytes_ = 0;
  std::size_t in_flight_dispatches_ = 0;
  bool outgoing_drain_active_ = false;
  bool initializing_ = false;
  bool shutting_down_ = false;
  bool initialized_ = false;
  bool accepting_messages_ = false;
  std::atomic<uint64_t> next_browser_window_generation_{1};
};

} // namespace runtime
} // namespace mocktail

#endif // MOCKTAIL_RUNTIME_ROBLOX_BROWSER_SERVICE_BRIDGE_H_
