#ifndef MOCKTAIL_RUNTIME_ROBLOX_WEB_VIEW_BRIDGE_H_
#define MOCKTAIL_RUNTIME_ROBLOX_WEB_VIEW_BRIDGE_H_

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

#include "mocktail/status.h"
#include "runtime/roblox_game_session_native_adapter.h"
#include "runtime/webview_helper_launcher.h"

namespace mocktail {
namespace runtime {

inline constexpr std::size_t kMaximumRobloxWebViewJsonBytes = 64 * 1024;
inline constexpr std::size_t kMaximumRobloxWebViewUrlBytes = 16 * 1024;
inline constexpr std::size_t kMaximumRobloxDataModelNotificationTypeBytes = 128;
inline constexpr char kRobloxOpenCustomWebViewNotification[] =
    "OPEN_CUSTOM_WEBVIEW";
inline constexpr char kRobloxOpenCaptchaViewNotification[] =
    "OPEN_CAPTCHA_VIEW";
inline constexpr char kRobloxViewProfileNotification[] = "VIEW_PROFILE";
inline constexpr char kRobloxPurchaseRobuxNotification[] = "PURCHASE_ROBUX";

// Owned subset of WebViewProtocol's openWindow payload. The URL is copied
// before the MessageBus RawCallback returns, so no JNI local reference escapes.
struct RobloxWebViewOpenRequest {
  std::string url;
  std::string title;
  std::optional<std::string> search_type;
  std::optional<bool> is_visible;
  std::optional<std::string> window_type;
  std::optional<bool> show_domain_as_title;
  std::optional<bool> back_button_visible;
  std::optional<bool> hide_header;
};

// WebViewProtocol.mutateWindow is a partial update. Field absence must remain
// distinguishable from an explicitly supplied empty string or false value.
struct RobloxWebViewMutationRequest {
  std::optional<std::string> url;
  std::optional<std::string> title;
  std::optional<std::string> search_type;
  std::optional<bool> is_visible;
  std::optional<std::string> window_type;
  std::optional<bool> show_domain_as_title;
};

Status ParseRobloxWebViewOpenJson(const std::string &json,
                                  RobloxWebViewOpenRequest *request);
Status ParseRobloxWebViewMutationJson(const std::string& json,
                                      RobloxWebViewMutationRequest* request);
Status ParseRobloxCaptchaOpenJson(const std::string& json,
                                  RobloxWebViewOpenRequest* request);
Status ParseRobloxProfileViewUrl(const std::string& url,
                                 RobloxWebViewOpenRequest* request);
Status ParseRobloxPurchaseRobuxNotification(const std::string& json,
                                            RobloxWebViewOpenRequest* request);

using DispatchRobloxWebViewOpenFn =
    Status (*)(void *context, const RobloxWebViewOpenRequest &request,
               WebViewHelperExitObserver exit_observer);
using DispatchRobloxWebViewMutateFn =
    Status (*)(void* context, const RobloxWebViewMutationRequest& request);
using DispatchRobloxWebViewCloseFn = Status (*)(void* context);
using DispatchRobloxWebViewCookieFn =
    Status (*)(void* context, std::string_view canonical_header);

struct RobloxWebViewSink {
  void *context = nullptr;
  DispatchRobloxWebViewOpenFn dispatch_open = nullptr;
  DispatchRobloxWebViewMutateFn dispatch_mutate = nullptr;
  DispatchRobloxWebViewCloseFn dispatch_close = nullptr;
  DispatchRobloxWebViewCookieFn dispatch_cookie = nullptr;

  bool valid() const {
    return dispatch_open != nullptr && dispatch_mutate != nullptr &&
           dispatch_close != nullptr && dispatch_cookie != nullptr;
  }
};

using GetWebViewOpenWindowIdFn = jstring (*)(JNIEnv *, jclass);
using GetWebViewMutateWindowIdFn = jstring (*)(JNIEnv*, jclass);
using GetWebViewCloseWindowIdFn = jstring (*)(JNIEnv*, jclass);
using GetWebViewHandleWindowCloseIdFn = jstring (*)(JNIEnv *, jclass);
using GetWebViewStringFn = jstring (*)(JNIEnv *, jclass);
using GetWebViewMessageIdFn = jstring (*)(JNIEnv *, jclass, jstring, jstring);
using InitializeAndroidWebViewProtocolFn = void (*)(JNIEnv *, jclass);
using SubscribeWebViewRawFn = jobject (*)(JNIEnv *, jobject, jstring, jobject,
                                          jboolean);
using DeleteWebViewConnectionFn = void (*)(JNIEnv *, jobject, jlong);
using SetWebViewRequestHandlerRawFn = void (*)(JNIEnv *, jobject, jstring,
                                               jstring, jobject);
using ClearWebViewRequestHandlerFn = void (*)(JNIEnv *, jobject, jstring,
                                              jstring);
using PublishWebViewRawFn = void (*)(JNIEnv *, jobject, jstring, jstring);
using BroadcastWebViewDataModelFocusFn = void (*)(JNIEnv *, jclass, jstring,
                                                  jstring, jstring);
using SignalWebViewJavascriptCallbackFn = void (*)(JNIEnv*, jclass, jstring);
using UpdateRobloxCookieSetHandlerFn =
    void (*)(JNIEnv*, jobject, jobject);

struct RobloxWebViewMessageBusSymbols {
  GetWebViewOpenWindowIdFn get_open_window_id = nullptr;
  GetWebViewHandleWindowCloseIdFn get_handle_window_close_id = nullptr;
  GetWebViewStringFn get_protocol_name = nullptr;
  GetWebViewStringFn get_is_available_id = nullptr;
  GetWebViewMessageIdFn get_message_id = nullptr;
  InitializeAndroidWebViewProtocolFn initialize_android_web_view_protocol =
      nullptr;
  SubscribeWebViewRawFn subscribe_raw = nullptr;
  DeleteWebViewConnectionFn delete_connection = nullptr;
  SetWebViewRequestHandlerRawFn set_request_handler_raw = nullptr;
  ClearWebViewRequestHandlerFn clear_request_handler = nullptr;
  PublishWebViewRawFn publish_raw = nullptr;
  BroadcastWebViewDataModelFocusFn broadcast_data_model_focus = nullptr;
  // Trailing fields preserve source compatibility for existing aggregate
  // initializers while the native resolver migrates to the complete protocol.
  GetWebViewMutateWindowIdFn get_mutate_window_id = nullptr;
  GetWebViewCloseWindowIdFn get_close_window_id = nullptr;
  SignalWebViewJavascriptCallbackFn signal_javascript_callback = nullptr;
  UpdateRobloxCookieSetHandlerFn update_cookie_set_handler = nullptr;

  bool complete() const {
    return get_open_window_id != nullptr &&
           get_handle_window_close_id != nullptr &&
           get_protocol_name != nullptr && get_is_available_id != nullptr &&
           get_message_id != nullptr &&
           initialize_android_web_view_protocol != nullptr &&
           subscribe_raw != nullptr && delete_connection != nullptr &&
           set_request_handler_raw != nullptr &&
           clear_request_handler != nullptr && publish_raw != nullptr &&
           broadcast_data_model_focus != nullptr &&
           get_mutate_window_id != nullptr && get_close_window_id != nullptr &&
           signal_javascript_callback != nullptr &&
           update_cookie_set_handler != nullptr;
  }
};

struct RobloxWebViewMessageBusObjects {
  jobject message_bus = nullptr;
  void *callback_factory_context = nullptr;
  jobject (*create_raw_callback)(void *factory_context,
                                 std::shared_ptr<void> callback_context,
                                 void (*run)(void *, JNIEnv *,
                                             jstring)) = nullptr;
  void (*clear_raw_callback)(void *factory_context, jobject callback) = nullptr;
  jobject (*create_request_handler)(void *factory_context,
                                    std::shared_ptr<void> callback_context,
                                    std::string (*run)(void *, JNIEnv *,
                                                       jstring)) = nullptr;
  void (*clear_request_handler)(void *factory_context,
                                jobject handler) = nullptr;
  bool (*set_platform_web_callbacks)(
      void* factory_context, std::shared_ptr<void> callback_context,
      void (*on_data_model_notification)(void*, JNIEnv*, jstring, jstring),
      void (*on_app_bridge_notification)(void*, JNIEnv*, jstring, jstring),
      void (*on_native_overlay)(void*, JNIEnv*, jstring, jstring),
      void (*on_open_web_activity)(void*, JNIEnv*, jstring, jstring),
      void (*on_sync_cookies)(void*, JNIEnv*, jstring),
      void (*on_set_cookie)(void*, JNIEnv*, jstring, jstring)) = nullptr;
  void (*clear_platform_web_callbacks)(void* factory_context) = nullptr;

  bool complete() const {
    return message_bus != nullptr && callback_factory_context != nullptr &&
           create_raw_callback != nullptr && clear_raw_callback != nullptr &&
           create_request_handler != nullptr &&
           clear_request_handler != nullptr &&
           set_platform_web_callbacks != nullptr &&
           clear_platform_web_callbacks != nullptr;
  }
};

// API-first adapter for WebViewProtocol's open, mutate, and close
// subscriptions. It owns no GUI toolkit: the sink owns the
// host-window policy.
class RobloxWebViewBridge final {
public:
  RobloxWebViewBridge(JniEnvironmentProvider environment,
                      RobloxWebViewMessageBusSymbols symbols,
                      RobloxWebViewMessageBusObjects objects,
                      RobloxWebViewSink sink);
  ~RobloxWebViewBridge();

  RobloxWebViewBridge(const RobloxWebViewBridge &) = delete;
  RobloxWebViewBridge &operator=(const RobloxWebViewBridge &) = delete;

  Status Initialize();
  Status Shutdown();

  Status HandleRawMessage(JNIEnv *env, jstring message);
  Status HandleOwnedMessage(std::string message);
  Status HandleMutateRawMessage(JNIEnv* env, jstring message);
  Status HandleOwnedMutation(std::string message);
  Status HandleCloseWindow();
  Status HandleDataModelNotification(JNIEnv *env, jstring type, jstring data);
  Status HandleAppBridgeNotification(JNIEnv* env, jstring type, jstring data);
  Status HandleNativeOverlay(JNIEnv* env, jstring title, jstring url);
  Status HandleOpenWebActivity(JNIEnv* env, jstring url, jstring title);
  Status SignalJavascriptCallback(const std::string& payload);
  Status DrainHostWindowEvents();

  bool initialized() const;

private:
 enum class SubscriptionKind : uint8_t {
   kOpenWindow,
   kMutateWindow,
   kCloseWindow,
 };

 static constexpr std::size_t kSubscriptionCount = 3;

 enum class OpenSource : uint8_t {
   kMessageBus,
   kDataModelNotification,
   kNativeOverlay,
   kWebActivity,
   kCaptcha,
 };

 enum class HostWindowEventType : uint8_t {
   kDataModelUnfocused,
   kDataModelFocused,
   kMessageBusWindowClose,
 };

 struct PendingHostWindowEvent {
   HostWindowEventType type = HostWindowEventType::kDataModelUnfocused;
   uint64_t generation = 0;
 };

  Status CheckJniException(JNIEnv *env, const char *operation) const;
  Status BroadcastDataModelFocus(JNIEnv *env, const char *state);
  Status HandleOwnedMessageFromSource(std::string message, OpenSource source);
  Status DispatchOpenRequest(RobloxWebViewOpenRequest request,
                             OpenSource source);
  void QueueHostWindowEvent(HostWindowEventType type, uint64_t generation);
  Status BeginDispatch();
  void EndDispatch();
  static void RawMessageCallback(void *context, JNIEnv *env, jstring message);
  static std::string AvailabilityCallback(void *context, JNIEnv *env,
                                          jstring message);
  static void DataModelNotificationCallback(void *context, JNIEnv *env,
                                            jstring type, jstring data);
  static void AppBridgeNotificationCallback(void* context, JNIEnv* env,
                                            jstring type, jstring data);
  static void NativeOverlayCallback(void* context, JNIEnv* env, jstring title,
                                    jstring url);
  static void OpenWebActivityCallback(void* context, JNIEnv* env, jstring url,
                                      jstring title);
  static void SyncCookiesCallback(void* context, JNIEnv* env, jstring cookie);
  static void SetCookieCallback(void* context, JNIEnv* env, jstring cookie,
                                jstring url);
  static void HostWindowExited(void *context);

  struct RawCallbackTarget;
  struct HostWindowCloseTarget;
  struct HostWindowExitContext;

  const JniEnvironmentProvider environment_;
  const RobloxWebViewMessageBusSymbols symbols_;
  const RobloxWebViewMessageBusObjects source_objects_;
  const RobloxWebViewSink sink_;

  mutable std::mutex mutex_;
  std::condition_variable dispatch_drained_;
  jobject message_bus_ = nullptr;
  std::array<jobject, kSubscriptionCount> raw_callbacks_{};
  std::array<jobject, kSubscriptionCount> connections_{};
  jobject availability_handler_ = nullptr;
  jobject cookie_protocol_ = nullptr;
  jobject cookie_set_handler_ = nullptr;
  jstring protocol_name_ = nullptr;
  jstring is_available_id_ = nullptr;
  jstring handle_window_close_message_id_ = nullptr;
  std::array<jlong, kSubscriptionCount> connection_handles_{};
  std::array<std::shared_ptr<RawCallbackTarget>, kSubscriptionCount>
      raw_callback_targets_{};
  std::shared_ptr<HostWindowCloseTarget> host_window_close_target_;
  std::size_t in_flight_dispatches_ = 0;
  bool initializing_ = false;
  bool shutting_down_ = false;
  bool initialized_ = false;
  bool accepting_messages_ = false;
  bool platform_web_callbacks_registered_ = false;
  bool cookie_set_handler_registered_ = false;
  std::atomic<uint64_t> next_host_window_generation_{1};
  std::mutex host_window_event_mutex_;
  std::deque<PendingHostWindowEvent> pending_host_window_events_;
};

} // namespace runtime
} // namespace mocktail

#endif // MOCKTAIL_RUNTIME_ROBLOX_WEB_VIEW_BRIDGE_H_
