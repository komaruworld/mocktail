#include <fcntl.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <gtk/gtk.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <webkit/webkit.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>

#include "runtime/runtime_config.h"
#include "runtime/webview_helper_launcher.h"
#include "webview/webview_helper_policy.h"

namespace {

constexpr std::size_t kMaximumRequestBytes =
    mocktail::runtime::kMaximumWebViewUrlBytes + 64;
constexpr std::size_t kMaximumQueuedHybridEvents = 64;
constexpr std::size_t kMaximumQueuedHybridBytes = 1024 * 1024;

using mocktail::webview::BoundedLogToken;
using mocktail::webview::BuildCallbackScript;
using mocktail::webview::BuildRobloxAndroidUserAgent;
using mocktail::webview::CaptchaEvent;
using mocktail::webview::CaptchaEventName;
using mocktail::webview::CaptchaEventType;
using mocktail::webview::EvaluateNavigationUri;
using mocktail::webview::ExtractExecuteRobloxCommand;
using mocktail::webview::ExtractRobloxWkHybridCommand;
using mocktail::webview::ParseCaptchaEvent;
using mocktail::webview::UriPolicyResult;

enum class PendingRobloxCookieOperation {
  kNone,
  kRetainPersistent,
  kSet,
  kClear,
};

struct AppState {
  GtkApplication* application = nullptr;
  WebKitNetworkSession* network_session = nullptr;
  std::string initial_url;
  std::string pending_navigation_url;
  std::string pending_roblox_cookie;
  std::deque<std::string> pending_hybrid_packets;
  std::size_t pending_hybrid_bytes = 0;
  GWeakRef primary_web_view;
  GWeakRef primary_window;
  guint termination_source = 0;
  guint control_source = 0;
  guint hybrid_write_source = 0;
  uint64_t cookie_generation = 0;
  PendingRobloxCookieOperation pending_cookie_operation =
      PendingRobloxCookieOperation::kNone;
  bool back_navigation_disabled = false;
  bool show_domain_as_title = false;
  bool startup_failed = false;
  bool terminating = false;
  bool cookie_sync_received = false;
  bool cookie_install_in_flight = false;
  bool initial_load_started = false;
};

struct CookieInstallContext {
  AppState* state = nullptr;
  uint64_t generation = 0;
};

struct SurfaceState {
  explicit SurfaceState(AppState* app_state, GtkWindow* surface_window,
                        bool primary_surface)
      : app(app_state), primary(primary_surface) {
    g_weak_ref_init(&window, G_OBJECT(surface_window));
  }

  ~SurfaceState() { g_weak_ref_clear(&window); }

  AppState* app = nullptr;
  GWeakRef window;
  bool primary = false;
};

struct CallbackConfirmation {
  CallbackConfirmation(GtkWindow* surface_window, CaptchaEventType event_type)
      : type(event_type) {
    g_weak_ref_init(&window, G_OBJECT(surface_window));
  }

  ~CallbackConfirmation() { g_weak_ref_clear(&window); }

  GWeakRef window;
  CaptchaEventType type = CaptchaEventType::kShown;
};

void BeginTermination(AppState* state, const char* reason);
void ApplyPendingRobloxCookie(AppState* state);

void ClearSensitiveString(std::string* value) {
  volatile char* bytes = value->empty() ? nullptr : value->data();
  for (std::size_t index = 0; index < value->size(); ++index) {
    bytes[index] = '\0';
  }
  value->clear();
}

bool ReadRequest(std::string *request) {
  request->clear();
  char buffer[1024];
  while (true) {
    const ssize_t count = read(STDIN_FILENO, buffer, sizeof(buffer));
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (count == 0) {
      return true;
    }
    if (request->size() + static_cast<std::size_t>(count) >
        kMaximumRequestBytes) {
      return false;
    }
    request->append(buffer, static_cast<std::size_t>(count));
  }
}

bool PrepareControlChannel() {
  int socket_type = 0;
  socklen_t socket_type_size = sizeof(socket_type);
  if (getsockopt(mocktail::runtime::kWebViewHelperControlDescriptor, SOL_SOCKET,
                 SO_TYPE, &socket_type, &socket_type_size) != 0 ||
      socket_type != SOCK_SEQPACKET) {
    return false;
  }
  const int flags =
      fcntl(mocktail::runtime::kWebViewHelperControlDescriptor, F_GETFL);
  const int descriptor_flags =
      fcntl(mocktail::runtime::kWebViewHelperControlDescriptor, F_GETFD);
  return flags >= 0 && descriptor_flags >= 0 &&
         fcntl(mocktail::runtime::kWebViewHelperControlDescriptor, F_SETFL,
               flags | O_NONBLOCK) == 0 &&
         fcntl(mocktail::runtime::kWebViewHelperControlDescriptor, F_SETFD,
               descriptor_flags | FD_CLOEXEC) == 0;
}

const char* LoadEventName(WebKitLoadEvent event) {
  switch (event) {
    case WEBKIT_LOAD_STARTED:
      return "started";
    case WEBKIT_LOAD_REDIRECTED:
      return "redirected";
    case WEBKIT_LOAD_COMMITTED:
      return "committed";
    case WEBKIT_LOAD_FINISHED:
      return "finished";
  }
  return "unknown";
}

const char* ProcessTerminationName(WebKitWebProcessTerminationReason reason) {
  switch (reason) {
    case WEBKIT_WEB_PROCESS_CRASHED:
      return "crashed";
    case WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT:
      return "memory-limit";
    case WEBKIT_WEB_PROCESS_TERMINATED_BY_API:
      return "api";
  }
  return "unknown";
}

bool EnsurePrivateDirectory(const char* path) {
  if (g_mkdir_with_parents(path, 0700) != 0) {
    return false;
  }
  struct stat status = {};
  if (lstat(path, &status) != 0 || !S_ISDIR(status.st_mode) ||
      S_ISLNK(status.st_mode)) {
    errno = ENOTDIR;
    return false;
  }
  return g_chmod(path, 0700) == 0;
}

bool EnsurePrivateCookieDatabase(const char* path) {
  const int descriptor =
      open(path, O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_RDWR, 0600);
  if (descriptor < 0) {
    return false;
  }
  struct stat status = {};
  const bool secured = fstat(descriptor, &status) == 0 &&
                       S_ISREG(status.st_mode) && fchmod(descriptor, 0600) == 0;
  close(descriptor);
  return secured;
}

bool InitializeNetworkSession(AppState* state) {
  gchar* data_directory =
      g_build_filename(g_get_user_data_dir(), "mocktail", "webview", nullptr);
  gchar* cache_directory =
      g_build_filename(g_get_user_cache_dir(), "mocktail", "webview", nullptr);
  if (data_directory == nullptr || cache_directory == nullptr ||
      !EnsurePrivateDirectory(data_directory) ||
      !EnsurePrivateDirectory(cache_directory)) {
    std::cerr << "[webview] cannot create private XDG WebKit storage: "
              << std::strerror(errno) << '\n';
    g_free(data_directory);
    g_free(cache_directory);
    return false;
  }

  gchar* cookie_database =
      g_build_filename(data_directory, "cookies.sqlite", nullptr);
  if (cookie_database == nullptr ||
      !EnsurePrivateCookieDatabase(cookie_database)) {
    std::cerr << "[webview] cannot secure persistent cookie database\n";
    g_free(cookie_database);
    g_free(data_directory);
    g_free(cache_directory);
    return false;
  }
  state->network_session =
      webkit_network_session_new(data_directory, cache_directory);
  g_free(data_directory);
  g_free(cache_directory);
  if (state->network_session == nullptr) {
    g_free(cookie_database);
    return false;
  }

  const char* system_proxy = std::getenv("MOCKTAIL_USE_SYSTEM_PROXY");
  const bool use_system_proxy =
      system_proxy != nullptr && system_proxy[0] != '\0' &&
      std::string_view(system_proxy) != "0";
  const char* proxy_host = std::getenv("MOCKTAIL_HTTP_PROXY_HOST");
  const char* proxy_port = std::getenv("MOCKTAIL_HTTP_PROXY_PORT");
  const char* proxy_scheme = std::getenv("MOCKTAIL_HTTP_PROXY_SCHEME");
  if (proxy_host != nullptr && proxy_port != nullptr) {
    const std::optional<mocktail::runtime::NetworkProxyConfig> proxy =
        mocktail::runtime::ParseNetworkProxyConfig(
            proxy_host, proxy_port,
            proxy_scheme != nullptr ? proxy_scheme : "http");
    if (proxy.has_value()) {
      const std::string proxy_url =
          mocktail::runtime::BuildNetworkProxyUrl(*proxy);
      WebKitNetworkProxySettings* settings =
          webkit_network_proxy_settings_new(proxy_url.c_str(), nullptr);
      if (settings == nullptr) {
        g_object_unref(state->network_session);
        state->network_session = nullptr;
        g_free(cookie_database);
        return false;
      }
      webkit_network_session_set_proxy_settings(
          state->network_session, WEBKIT_NETWORK_PROXY_MODE_CUSTOM, settings);
      webkit_network_proxy_settings_free(settings);
      std::cerr << "[webview] "
                << (use_system_proxy ? "normalized system" : "configured")
                << " proxy applied: " << proxy_url << '\n';
    }
  }

  WebKitCookieManager* cookies =
      webkit_network_session_get_cookie_manager(state->network_session);
  webkit_cookie_manager_set_persistent_storage(
      cookies, cookie_database, WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
  webkit_cookie_manager_set_accept_policy(cookies,
                                          WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS);
  g_free(cookie_database);

  // Android WebView permits the third-party cookies used by Roblox login and
  // CAPTCHA providers. TLS failures remain fail-closed at the network layer.
  webkit_network_session_set_itp_enabled(state->network_session, FALSE);
  webkit_network_session_set_persistent_credential_storage_enabled(
      state->network_session, TRUE);
  webkit_network_session_set_tls_errors_policy(state->network_session,
                                               WEBKIT_TLS_ERRORS_POLICY_FAIL);
  std::cerr << "[webview] persistent XDG network session ready\n";
  return true;
}

void StartInitialLoad(AppState* state) {
  if (!state->cookie_sync_received || state->cookie_install_in_flight) {
    return;
  }
  auto* web_view_object =
      static_cast<GObject*>(g_weak_ref_get(&state->primary_web_view));
  if (web_view_object == nullptr) {
    return;
  }
  std::string* target = !state->pending_navigation_url.empty()
                            ? &state->pending_navigation_url
                            : &state->initial_url;
  if (!target->empty()) {
    state->initial_load_started = true;
    webkit_web_view_load_uri(WEBKIT_WEB_VIEW(web_view_object), target->c_str());
    ClearSensitiveString(target);
  }
  g_object_unref(web_view_object);
}

void FinishRobloxCookieInstall(GObject* source, GAsyncResult* result,
                               gpointer user_data) {
  std::unique_ptr<CookieInstallContext> context(
      static_cast<CookieInstallContext*>(user_data));
  AppState* state = context->state;
  GError* error = nullptr;
  const bool installed = webkit_cookie_manager_add_cookie_finish(
      WEBKIT_COOKIE_MANAGER(source), result, &error);
  state->cookie_install_in_flight = false;
  if (context->generation != state->cookie_generation) {
    g_clear_error(&error);
    ApplyPendingRobloxCookie(state);
    return;
  }
  if (!installed) {
    std::cerr << "[webview] Roblox cookie installation failed\n";
    g_clear_error(&error);
    BeginTermination(state, "cookie installation failed");
    return;
  }
  state->pending_cookie_operation = PendingRobloxCookieOperation::kNone;
  std::cerr << "[webview] Roblox cookie installed before navigation\n";
  StartInitialLoad(state);
}

void FinishRobloxCookieDelete(GObject* source, GAsyncResult* result,
                              gpointer user_data) {
  std::unique_ptr<CookieInstallContext> context(
      static_cast<CookieInstallContext*>(user_data));
  AppState* state = context->state;
  GError* error = nullptr;
  const bool deleted = webkit_cookie_manager_delete_cookie_finish(
      WEBKIT_COOKIE_MANAGER(source), result, &error);
  state->cookie_install_in_flight = false;
  if (context->generation != state->cookie_generation) {
    g_clear_error(&error);
    ApplyPendingRobloxCookie(state);
    return;
  }
  if (!deleted) {
    std::cerr << "[webview] stale Roblox cookie deletion failed\n";
    g_clear_error(&error);
    BeginTermination(state, "cookie deletion failed");
    return;
  }
  state->pending_cookie_operation = PendingRobloxCookieOperation::kNone;
  std::cerr << "[webview] Roblox cookie deleted before navigation\n";
  StartInitialLoad(state);
}

void ApplyPendingRobloxCookie(AppState* state) {
  if (!state->cookie_sync_received || state->network_session == nullptr ||
      state->cookie_install_in_flight) {
    return;
  }
  if (state->pending_cookie_operation ==
      PendingRobloxCookieOperation::kRetainPersistent) {
    state->pending_cookie_operation = PendingRobloxCookieOperation::kNone;
    std::cerr << "[webview] persistent Roblox cookies retained before guest "
                 "navigation\n";
    StartInitialLoad(state);
    return;
  }
  if (state->pending_cookie_operation == PendingRobloxCookieOperation::kNone) {
    return;
  }
  if (state->pending_cookie_operation == PendingRobloxCookieOperation::kClear) {
    SoupCookie* stale_cookie =
        soup_cookie_new(".ROBLOSECURITY", "", ".roblox.com", "/", -1);
    if (stale_cookie == nullptr) {
      BeginTermination(state, "cookie deletion setup failed");
      return;
    }
    WebKitCookieManager* manager =
        webkit_network_session_get_cookie_manager(state->network_session);
    state->cookie_install_in_flight = true;
    auto* context = new CookieInstallContext{state, state->cookie_generation};
    webkit_cookie_manager_delete_cookie(manager, stale_cookie, nullptr,
                                        FinishRobloxCookieDelete, context);
    soup_cookie_free(stale_cookie);
    return;
  }
  if (state->pending_cookie_operation != PendingRobloxCookieOperation::kSet ||
      state->pending_roblox_cookie.empty()) {
    BeginTermination(state, "cookie synchronization state is invalid");
    return;
  }
  SoupCookie* cookie =
      soup_cookie_new(".ROBLOSECURITY", state->pending_roblox_cookie.c_str(),
                      ".roblox.com", "/", -1);
  if (cookie == nullptr) {
    BeginTermination(state, "cookie construction failed");
    return;
  }
  soup_cookie_set_secure(cookie, TRUE);
  soup_cookie_set_http_only(cookie, TRUE);
  soup_cookie_set_same_site_policy(cookie, SOUP_SAME_SITE_POLICY_LAX);
  WebKitCookieManager* manager =
      webkit_network_session_get_cookie_manager(state->network_session);
  state->cookie_install_in_flight = true;
  auto* context = new CookieInstallContext{state, state->cookie_generation};
  webkit_cookie_manager_add_cookie(manager, cookie, nullptr,
                                   FinishRobloxCookieInstall, context);
  soup_cookie_free(cookie);
  ClearSensitiveString(&state->pending_roblox_cookie);
}

void BeginTermination(AppState* state, const char* reason) {
  if (state->terminating || state->application == nullptr) {
    return;
  }
  state->terminating = true;
  std::cerr << "[webview] " << reason << "; closing WebKit surfaces\n";

  GList* windows = g_list_copy(gtk_application_get_windows(state->application));
  for (GList* item = windows; item != nullptr; item = item->next) {
    g_object_ref(item->data);
  }
  for (GList* item = windows; item != nullptr; item = item->next) {
    gtk_window_close(GTK_WINDOW(item->data));
  }
  g_list_free_full(windows, g_object_unref);
  g_application_quit(G_APPLICATION(state->application));
}

gboolean OnTerminationSignal(gpointer user_data) {
  auto* state = static_cast<AppState*>(user_data);
  state->termination_source = 0;
  BeginTermination(state, "SIGTERM received");
  return G_SOURCE_REMOVE;
}

void FinishCallbackConfirmation(GObject* source_object, GAsyncResult* result,
                                gpointer user_data) {
  auto* confirmation = static_cast<CallbackConfirmation*>(user_data);
  GError* error = nullptr;
  JSCValue* value = webkit_web_view_evaluate_javascript_finish(
      WEBKIT_WEB_VIEW(source_object), result, &error);
  const bool confirmed = error == nullptr && value != nullptr &&
                         jsc_value_is_boolean(value) &&
                         jsc_value_to_boolean(value);
  if (error != nullptr) {
    std::cerr << "[webview] hybrid callback failed feature="
              << CaptchaEventName(confirmation->type) << " domain="
              << BoundedLogToken(g_quark_to_string(error->domain), "unknown")
              << " code=" << error->code << '\n';
    g_error_free(error);
  } else {
    std::cerr << "[webview] hybrid callback feature="
              << CaptchaEventName(confirmation->type)
              << " confirmed=" << (confirmed ? "yes" : "no") << '\n';
  }
  if (value != nullptr) {
    g_object_unref(value);
  }

  auto* window_object =
      static_cast<GObject*>(g_weak_ref_get(&confirmation->window));
  if (confirmed && confirmation->type == CaptchaEventType::kSuccess &&
      window_object != nullptr) {
    gtk_window_close(GTK_WINDOW(window_object));
  }
  if (window_object != nullptr) {
    g_object_unref(window_object);
  }
  delete confirmation;
}

void HandleHybridCommand(WebKitUserContentManager* manager,
                         SurfaceState* surface, std::string_view command) {
  CaptchaEvent event;
  if (!ParseCaptchaEvent(command, &event)) {
    return;
  }
  std::cerr << "[webview] hybrid event feature=" << CaptchaEventName(event.type)
            << " callback=" << (event.callback_id.empty() ? "missing" : "yes")
            << '\n';
  if (event.callback_id.empty()) {
    std::cerr << "[webview] hybrid callback not confirmed feature="
              << CaptchaEventName(event.type) << " reason=missing-id\n";
    return;
  }

  auto* window_object = static_cast<GObject*>(g_weak_ref_get(&surface->window));
  if (window_object == nullptr) {
    return;
  }
  auto* confirmation =
      new CallbackConfirmation(GTK_WINDOW(window_object), event.type);
  g_object_unref(window_object);
  const std::string script = BuildCallbackScript(event.callback_id);
  WebKitWebView* web_view = WEBKIT_WEB_VIEW(
      g_object_get_data(G_OBJECT(manager), "mocktail-web-view"));
  if (web_view == nullptr) {
    delete confirmation;
    return;
  }
  webkit_web_view_evaluate_javascript(
      web_view, script.data(), static_cast<gssize>(script.size()), nullptr,
      nullptr, nullptr, FinishCallbackConfirmation, confirmation);
}

gboolean FlushHybridEvents(gint descriptor, GIOCondition condition,
                           gpointer user_data) {
  auto* state = static_cast<AppState*>(user_data);
  if ((condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) != 0) {
    BeginTermination(state, "hybrid channel disconnected");
    state->hybrid_write_source = 0;
    return G_SOURCE_REMOVE;
  }
  while (!state->pending_hybrid_packets.empty()) {
    const std::string& packet = state->pending_hybrid_packets.front();
    const std::size_t packet_size = packet.size();
    ssize_t count = -1;
    do {
      count = send(descriptor, packet.data(), packet.size(),
                   MSG_DONTWAIT | MSG_NOSIGNAL);
    } while (count < 0 && errno == EINTR);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      return G_SOURCE_CONTINUE;
    }
    state->pending_hybrid_bytes -= packet_size;
    ClearSensitiveString(&state->pending_hybrid_packets.front());
    state->pending_hybrid_packets.pop_front();
    if (count != static_cast<ssize_t>(packet_size)) {
      BeginTermination(state, "hybrid channel write failed");
      state->hybrid_write_source = 0;
      return G_SOURCE_REMOVE;
    }
  }
  state->hybrid_write_source = 0;
  return G_SOURCE_REMOVE;
}

bool QueueHelperEvent(AppState* state,
                      mocktail::runtime::WebViewHelperEventType type,
                      std::string_view command) {
  std::string packet;
  if (!mocktail::runtime::EncodeWebViewHelperEventPacket(type, command,
                                                         &packet)) {
    return false;
  }
  if (state->pending_hybrid_packets.size() >= kMaximumQueuedHybridEvents ||
      packet.size() > kMaximumQueuedHybridBytes - state->pending_hybrid_bytes) {
    ClearSensitiveString(&packet);
    return false;
  }
  state->pending_hybrid_bytes += packet.size();
  state->pending_hybrid_packets.push_back(std::move(packet));
  if (state->hybrid_write_source == 0) {
    state->hybrid_write_source = g_unix_fd_add(
        mocktail::runtime::kWebViewHelperControlDescriptor,
        static_cast<GIOCondition>(G_IO_OUT | G_IO_HUP | G_IO_ERR | G_IO_NVAL),
        FlushHybridEvents, state);
  }
  return state->hybrid_write_source != 0;
}

bool ForwardHybridEvent(AppState* state,
                        mocktail::runtime::WebViewHelperEventType type,
                        std::string_view command) {
  return QueueHelperEvent(state, type, command);
}

bool IsPrivilegedBridgeAllowed(WebKitUserContentManager* manager) {
  auto* web_view = static_cast<WebKitWebView*>(
      g_object_get_data(G_OBJECT(manager), "mocktail-web-view"));
  return web_view != nullptr &&
         EvaluateNavigationUri(webkit_web_view_get_uri(web_view))
             .privileged_bridge_allowed;
}

void OnExecuteRobloxMessage(WebKitUserContentManager* manager, JSCValue* value,
                            gpointer user_data) {
  if (!IsPrivilegedBridgeAllowed(manager)) {
    std::cerr << "[webview] rejected executeRoblox from untrusted origin\n";
    return;
  }
  std::string command;
  if (!ExtractExecuteRobloxCommand(value, &command)) {
    std::cerr << "[webview] rejected executeRoblox object command\n";
    return;
  }
  auto* surface = static_cast<SurfaceState*>(user_data);
  if (!ForwardHybridEvent(
          surface->app,
          mocktail::runtime::WebViewHelperEventType::kExecuteRoblox, command)) {
    std::cerr << "[webview] could not forward executeRoblox command\n";
  }
  HandleHybridCommand(manager, surface, command);
  ClearSensitiveString(&command);
}

void OnRobloxWkHybridMessage(WebKitUserContentManager* manager, JSCValue* value,
                             gpointer user_data) {
  if (!IsPrivilegedBridgeAllowed(manager)) {
    std::cerr << "[webview] rejected RobloxWKHybrid from untrusted origin\n";
    return;
  }
  std::string command;
  if (!ExtractRobloxWkHybridCommand(value, &command)) {
    std::cerr << "[webview] rejected RobloxWKHybrid command envelope\n";
    return;
  }
  auto* surface = static_cast<SurfaceState*>(user_data);
  if (!ForwardHybridEvent(
          surface->app,
          mocktail::runtime::WebViewHelperEventType::kRobloxWkHybrid,
          command)) {
    std::cerr << "[webview] could not forward RobloxWKHybrid command\n";
  }
  HandleHybridCommand(manager, surface, command);
  ClearSensitiveString(&command);
}

void OnCompatibilityScriptMessage(WebKitUserContentManager* manager,
                                  JSCValue* value, gpointer user_data) {
  if (!IsPrivilegedBridgeAllowed(manager)) {
    std::cerr << "[webview] rejected compatibility bridge from untrusted "
                 "origin\n";
    return;
  }
  if (!jsc_value_is_string(value)) {
    std::cerr << "[webview] rejected compatibility bridge command\n";
    return;
  }
  GBytes* bytes = jsc_value_to_string_as_bytes(value);
  if (bytes == nullptr) {
    return;
  }
  gsize size = 0;
  const auto* data = static_cast<const char*>(g_bytes_get_data(bytes, &size));
  if (data == nullptr || size == 0 ||
      size > mocktail::webview::kMaximumHybridCommandBytes) {
    std::cerr << "[webview] rejected empty or oversized compatibility "
                 "command\n";
    g_bytes_unref(bytes);
    return;
  }
  auto* surface = static_cast<SurfaceState*>(user_data);
  const std::string_view command(data, size);
  if (!ForwardHybridEvent(
          surface->app,
          mocktail::runtime::WebViewHelperEventType::kExecuteRoblox, command)) {
    std::cerr << "[webview] could not forward compatibility command\n";
  }
  HandleHybridCommand(manager, surface, command);
  g_bytes_unref(bytes);
}

void UpdateDomainTitle(WebKitWebView* web_view, SurfaceState* surface) {
  if (!surface->app->show_domain_as_title) {
    return;
  }
  const UriPolicyResult policy =
      EvaluateNavigationUri(webkit_web_view_get_uri(web_view));
  if (!policy.allowed || policy.host.empty()) {
    return;
  }
  auto* window_object = static_cast<GObject*>(g_weak_ref_get(&surface->window));
  if (window_object != nullptr) {
    gtk_window_set_title(GTK_WINDOW(window_object), policy.host.c_str());
    g_object_unref(window_object);
  }
}

void OnLoadChanged(WebKitWebView* web_view, WebKitLoadEvent event,
                   gpointer user_data) {
  const UriPolicyResult policy =
      EvaluateNavigationUri(webkit_web_view_get_uri(web_view));
  std::cerr << "[webview] load stage=" << LoadEventName(event)
            << " scheme=" << policy.scheme << " host=" << policy.host << '\n';
  if (event == WEBKIT_LOAD_COMMITTED || event == WEBKIT_LOAD_FINISHED) {
    UpdateDomainTitle(web_view, static_cast<SurfaceState*>(user_data));
  }
}

gboolean OnLoadFailed(WebKitWebView* web_view, WebKitLoadEvent event,
                      const gchar* failing_uri, GError* error,
                      gpointer user_data) {
  const UriPolicyResult policy = EvaluateNavigationUri(failing_uri);
  std::cerr << "[webview] load failed stage=" << LoadEventName(event)
            << " scheme=" << policy.scheme << " host=" << policy.host
            << " domain="
            << BoundedLogToken(error == nullptr
                                   ? nullptr
                                   : g_quark_to_string(error->domain),
                               "unknown")
            << " code=" << (error == nullptr ? 0 : error->code) << '\n';
  return FALSE;
}

gboolean OnTlsFailure(WebKitWebView* web_view, const gchar* failing_uri,
                      GTlsCertificate* certificate, GTlsCertificateFlags errors,
                      gpointer user_data) {
  const UriPolicyResult policy = EvaluateNavigationUri(failing_uri);
  std::cerr << "[webview] TLS failure scheme=" << policy.scheme
            << " host=" << policy.host
            << " flags=" << static_cast<unsigned int>(errors) << '\n';
  return FALSE;
}

void OnWebProcessTerminated(WebKitWebView* web_view,
                            WebKitWebProcessTerminationReason reason,
                            gpointer user_data) {
  std::cerr << "[webview] web process terminated reason="
            << ProcessTerminationName(reason) << '\n';
}

gboolean OnDecidePolicy(WebKitWebView* web_view, WebKitPolicyDecision* decision,
                        WebKitPolicyDecisionType type, gpointer user_data) {
  if (type != WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION &&
      type != WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {
    return FALSE;
  }
  auto* navigation_decision = WEBKIT_NAVIGATION_POLICY_DECISION(decision);
  WebKitNavigationAction* action =
      webkit_navigation_policy_decision_get_navigation_action(
          navigation_decision);
  WebKitURIRequest* request =
      action == nullptr ? nullptr
                        : webkit_navigation_action_get_request(action);
  const char* uri =
      request == nullptr ? nullptr : webkit_uri_request_get_uri(request);
  const UriPolicyResult policy = EvaluateNavigationUri(uri);
  if (!policy.allowed) {
    std::cerr << "[webview] navigation blocked scheme=" << policy.scheme
              << " host=" << policy.host << '\n';
    webkit_policy_decision_ignore(decision);
    return TRUE;
  }
  webkit_policy_decision_use(decision);
  return TRUE;
}

void ConfigureWebView(WebKitWebView* web_view, const AppState* app) {
  WebKitSettings* settings = webkit_web_view_get_settings(web_view);
  webkit_settings_set_enable_javascript(settings, TRUE);
  webkit_settings_set_enable_page_cache(settings, TRUE);
  webkit_settings_set_javascript_can_open_windows_automatically(settings, TRUE);
  webkit_settings_set_enable_back_forward_navigation_gestures(
      settings, !app->back_navigation_disabled);
  const std::string user_agent = BuildRobloxAndroidUserAgent();
  webkit_settings_set_user_agent(settings, user_agent.c_str());
}

void FinishControlledJavaScript(GObject* source_object, GAsyncResult* result,
                                gpointer user_data) {
  GError* error = nullptr;
  JSCValue* value = webkit_web_view_evaluate_javascript_finish(
      WEBKIT_WEB_VIEW(source_object), result, &error);
  if (error != nullptr) {
    std::cerr << "[webview] controlled JavaScript failed domain="
              << BoundedLogToken(g_quark_to_string(error->domain), "unknown")
              << " code=" << error->code << '\n';
    g_error_free(error);
  } else {
    std::cerr << "[webview] controlled JavaScript completed\n";
  }
  if (value != nullptr) {
    g_object_unref(value);
  }
}

bool ApplyControlCommand(
    AppState* state, mocktail::runtime::WebViewHelperControlCommand* command) {
  using mocktail::runtime::WebViewHelperControlOperation;
  auto* web_view_object =
      static_cast<GObject*>(g_weak_ref_get(&state->primary_web_view));
  auto* window_object =
      static_cast<GObject*>(g_weak_ref_get(&state->primary_window));
  WebKitWebView* web_view =
      web_view_object == nullptr ? nullptr : WEBKIT_WEB_VIEW(web_view_object);
  GtkWindow* window =
      window_object == nullptr ? nullptr : GTK_WINDOW(window_object);
  bool keep_running = true;

  switch (command->operation) {
    case WebViewHelperControlOperation::kLoadUrl:
      if (web_view != nullptr &&
          g_utf8_validate(command->payload.data(), command->payload.size(),
                          nullptr)) {
        ClearSensitiveString(&state->pending_navigation_url);
        state->pending_navigation_url = command->payload;
        StartInitialLoad(state);
      } else {
        std::cerr << "[webview] rejected URL control without a valid surface\n";
      }
      break;
    case WebViewHelperControlOperation::kSetTitle:
      if (window != nullptr &&
          g_utf8_validate(command->payload.data(), command->payload.size(),
                          nullptr)) {
        gtk_window_set_title(window, command->payload.c_str());
      } else {
        std::cerr
            << "[webview] rejected title control without a valid surface\n";
      }
      break;
    case WebViewHelperControlOperation::kSetVisible:
      if (window == nullptr) {
        std::cerr
            << "[webview] rejected visibility control without a surface\n";
      } else if (command->visible) {
        gtk_window_present(window);
      } else {
        gtk_widget_set_visible(GTK_WIDGET(window), FALSE);
      }
      break;
    case WebViewHelperControlOperation::kEvaluateJavaScript:
      if (web_view != nullptr &&
          g_utf8_validate(command->payload.data(), command->payload.size(),
                          nullptr)) {
        webkit_web_view_evaluate_javascript(
            web_view, command->payload.data(),
            static_cast<gssize>(command->payload.size()), nullptr, nullptr,
            nullptr, FinishControlledJavaScript, nullptr);
      } else {
        std::cerr << "[webview] rejected JavaScript control without a valid "
                     "surface\n";
      }
      break;
    case WebViewHelperControlOperation::kSetBackNavigationDisabled:
      state->back_navigation_disabled = command->visible;
      if (web_view != nullptr) {
        webkit_settings_set_enable_back_forward_navigation_gestures(
            webkit_web_view_get_settings(web_view), !command->visible);
      }
      break;
    case WebViewHelperControlOperation::kSetShowDomainAsTitle:
      state->show_domain_as_title = command->visible;
      if (web_view != nullptr) {
        auto* surface = static_cast<SurfaceState*>(
            g_object_get_data(web_view_object, "mocktail-surface-state"));
        if (surface != nullptr) {
          UpdateDomainTitle(web_view, surface);
        }
      }
      break;
    case WebViewHelperControlOperation::kSetRobloxCookie:
    case WebViewHelperControlOperation::kClearRobloxCookie:
    case WebViewHelperControlOperation::kRetainPersistentRobloxCookie:
      ++state->cookie_generation;
      if (state->cookie_generation == 0) {
        BeginTermination(state, "cookie generation exhausted");
        keep_running = false;
        break;
      }
      ClearSensitiveString(&state->pending_roblox_cookie);
      if (command->operation ==
          WebViewHelperControlOperation::kSetRobloxCookie) {
        state->pending_roblox_cookie = command->payload;
        state->pending_cookie_operation = PendingRobloxCookieOperation::kSet;
      } else if (command->operation ==
                 WebViewHelperControlOperation::kClearRobloxCookie) {
        state->pending_cookie_operation = PendingRobloxCookieOperation::kClear;
      } else {
        state->pending_cookie_operation =
            PendingRobloxCookieOperation::kRetainPersistent;
      }
      state->cookie_sync_received = true;
      ApplyPendingRobloxCookie(state);
      break;
    case WebViewHelperControlOperation::kClose:
      BeginTermination(state, "control close received");
      keep_running = false;
      break;
  }

  if (window_object != nullptr) {
    g_object_unref(window_object);
  }
  if (web_view_object != nullptr) {
    g_object_unref(web_view_object);
  }
  return keep_running;
}

gboolean OnControlChannel(gint descriptor, GIOCondition condition,
                          gpointer user_data) {
  auto* state = static_cast<AppState*>(user_data);
  std::array<char, mocktail::runtime::kMaximumWebViewControlPacketBytes>
      packet{};
  while (true) {
    const ssize_t count = recv(descriptor, packet.data(), packet.size(),
                               MSG_DONTWAIT | MSG_TRUNC);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        break;
      }
      std::cerr << "[webview] control channel read failed: "
                << std::strerror(errno) << '\n';
      state->control_source = 0;
      BeginTermination(state, "control channel failed");
      return G_SOURCE_REMOVE;
    }
    if (count == 0) {
      state->control_source = 0;
      BeginTermination(state, "control channel closed");
      return G_SOURCE_REMOVE;
    }
    if (count > static_cast<ssize_t>(packet.size())) {
      std::cerr << "[webview] rejected oversized control packet\n";
      continue;
    }

    mocktail::runtime::WebViewHelperControlCommand command;
    std::string error;
    if (!mocktail::runtime::DecodeWebViewHelperControlPacket(
            std::string_view(packet.data(), static_cast<std::size_t>(count)),
            &command, &error)) {
      std::cerr << "[webview] rejected malformed control packet: " << error
                << '\n';
      continue;
    }
    const bool keep_running = ApplyControlCommand(state, &command);
    ClearSensitiveString(&command.payload);
    if (!keep_running) {
      state->control_source = 0;
      return G_SOURCE_REMOVE;
    }
  }

  if ((condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) != 0) {
    state->control_source = 0;
    BeginTermination(state, "control channel disconnected");
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

void DestroySurfaceState(gpointer data) {
  delete static_cast<SurfaceState*>(data);
}

WebKitWebView* CreateSurface(AppState* app, WebKitWebView* related_view);

GtkWidget* OnCreatePopup(WebKitWebView* parent,
                         WebKitNavigationAction* navigation_action,
                         gpointer user_data) {
  auto* surface = static_cast<SurfaceState*>(user_data);
  WebKitWebView* popup = CreateSurface(surface->app, parent);
  return popup == nullptr ? nullptr : GTK_WIDGET(popup);
}

void OnReadyToShow(WebKitWebView* web_view, gpointer user_data) {
  auto* surface = static_cast<SurfaceState*>(user_data);
  auto* window_object = static_cast<GObject*>(g_weak_ref_get(&surface->window));
  if (window_object != nullptr) {
    gtk_window_present(GTK_WINDOW(window_object));
    g_object_unref(window_object);
  }
}

void OnWebViewClose(WebKitWebView* web_view, gpointer user_data) {
  auto* surface = static_cast<SurfaceState*>(user_data);
  if (surface->primary) {
    BeginTermination(surface->app, "primary WebView requested close");
    return;
  }
  auto* window_object = static_cast<GObject*>(g_weak_ref_get(&surface->window));
  if (window_object != nullptr) {
    gtk_window_close(GTK_WINDOW(window_object));
    g_object_unref(window_object);
  }
}

gboolean OnWindowCloseRequest(GtkWindow* window, gpointer user_data) {
  auto* surface = static_cast<SurfaceState*>(user_data);
  if (surface->primary && !surface->app->terminating) {
    BeginTermination(surface->app, "primary WebView window closed");
  }
  return FALSE;
}

WebKitWebView* CreateSurface(AppState* app, WebKitWebView* related_view) {
  WebKitUserContentManager* content_manager = webkit_user_content_manager_new();
  WebKitWebView* web_view = nullptr;
  if (related_view == nullptr) {
    web_view = WEBKIT_WEB_VIEW(g_object_new(
        WEBKIT_TYPE_WEB_VIEW, "network-session", app->network_session,
        "user-content-manager", content_manager, nullptr));
  } else {
    web_view = WEBKIT_WEB_VIEW(
        g_object_new(WEBKIT_TYPE_WEB_VIEW, "related-view", related_view,
                     "user-content-manager", content_manager, nullptr));
  }
  g_object_unref(content_manager);
  if (web_view == nullptr ||
      webkit_web_view_get_network_session(web_view) != app->network_session) {
    std::cerr << "[webview] failed to create a shared-session WebView\n";
    if (web_view != nullptr) {
      g_object_unref(web_view);
    }
    return nullptr;
  }

  GtkWidget* window = gtk_application_window_new(app->application);
  gtk_window_set_title(GTK_WINDOW(window), "Roblox");
  gtk_window_set_default_size(GTK_WINDOW(window),
                              related_view == nullptr ? 1100 : 900,
                              related_view == nullptr ? 760 : 720);
  if (related_view != nullptr) {
    auto* parent_surface = static_cast<SurfaceState*>(
        g_object_get_data(G_OBJECT(related_view), "mocktail-surface-state"));
    if (parent_surface != nullptr) {
      auto* parent_window =
          static_cast<GObject*>(g_weak_ref_get(&parent_surface->window));
      if (parent_window != nullptr) {
        gtk_window_set_transient_for(GTK_WINDOW(window),
                                     GTK_WINDOW(parent_window));
        g_object_unref(parent_window);
      }
    }
  }
  gtk_window_set_child(GTK_WINDOW(window), GTK_WIDGET(web_view));

  auto* surface =
      new SurfaceState(app, GTK_WINDOW(window), related_view == nullptr);
  g_object_set_data_full(G_OBJECT(web_view), "mocktail-surface-state", surface,
                         DestroySurfaceState);
  WebKitUserContentManager* manager =
      webkit_web_view_get_user_content_manager(web_view);
  g_object_set_data(G_OBJECT(manager), "mocktail-web-view", web_view);
  g_signal_connect(manager, "script-message-received::executeRoblox",
                   G_CALLBACK(OnExecuteRobloxMessage), surface);
  g_signal_connect(manager, "script-message-received::RobloxWKHybrid",
                   G_CALLBACK(OnRobloxWkHybridMessage), surface);
  g_signal_connect(manager, "script-message-received::mocktailRobloxBridge",
                   G_CALLBACK(OnCompatibilityScriptMessage), surface);
  const bool handlers_registered =
      webkit_user_content_manager_register_script_message_handler(
          manager, mocktail::webview::kExecuteRobloxHandler, nullptr) &&
      webkit_user_content_manager_register_script_message_handler(
          manager, mocktail::webview::kRobloxWkHybridHandler, nullptr) &&
      webkit_user_content_manager_register_script_message_handler(
          manager, mocktail::webview::kCompatibilityHandler, nullptr);
  if (!handlers_registered) {
    std::cerr << "[webview] failed to register Sober-compatible bridge "
                 "handlers\n";
    gtk_window_destroy(GTK_WINDOW(window));
    return nullptr;
  }
  WebKitUserScript* bridge_script = webkit_user_script_new(
      mocktail::webview::AndroidBridgeSource(),
      WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
      WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, nullptr, nullptr);
  webkit_user_content_manager_add_script(manager, bridge_script);
  webkit_user_script_unref(bridge_script);

  ConfigureWebView(web_view, app);
  g_signal_connect(web_view, "load-changed", G_CALLBACK(OnLoadChanged),
                   surface);
  g_signal_connect(web_view, "load-failed", G_CALLBACK(OnLoadFailed), surface);
  g_signal_connect(web_view, "load-failed-with-tls-errors",
                   G_CALLBACK(OnTlsFailure), surface);
  g_signal_connect(web_view, "web-process-terminated",
                   G_CALLBACK(OnWebProcessTerminated), surface);
  g_signal_connect(web_view, "decide-policy", G_CALLBACK(OnDecidePolicy),
                   surface);
  g_signal_connect(web_view, "create", G_CALLBACK(OnCreatePopup), surface);
  g_signal_connect(web_view, "ready-to-show", G_CALLBACK(OnReadyToShow),
                   surface);
  g_signal_connect(web_view, "close", G_CALLBACK(OnWebViewClose), surface);
  g_signal_connect(window, "close-request", G_CALLBACK(OnWindowCloseRequest),
                   surface);
  return web_view;
}

void Activate(GtkApplication* application, gpointer user_data) {
  auto* state = static_cast<AppState*>(user_data);
  state->application = application;
  if (!InitializeNetworkSession(state)) {
    state->startup_failed = true;
    g_application_quit(G_APPLICATION(application));
    return;
  }
  WebKitWebView* web_view = CreateSurface(state, nullptr);
  if (web_view == nullptr) {
    state->startup_failed = true;
    g_application_quit(G_APPLICATION(application));
    return;
  }

  g_weak_ref_set(&state->primary_web_view, G_OBJECT(web_view));

  ApplyPendingRobloxCookie(state);
  auto* surface = static_cast<SurfaceState*>(
      g_object_get_data(G_OBJECT(web_view), "mocktail-surface-state"));
  auto* window_object = static_cast<GObject*>(g_weak_ref_get(&surface->window));
  if (window_object != nullptr) {
    g_weak_ref_set(&state->primary_window, window_object);
    gtk_window_present(GTK_WINDOW(window_object));
    g_object_unref(window_object);
  }
  if (!QueueHelperEvent(
          state, mocktail::runtime::WebViewHelperEventType::kReady, {})) {
    state->startup_failed = true;
    BeginTermination(state, "could not publish helper readiness");
  }
}

} // namespace

int main(int argc, char *argv[]) {
  if (argc != 1) {
    std::cerr << "mocktail_webview_helper accepts its request only on stdin\n";
    return EXIT_FAILURE;
  }
  std::string request;
  AppState state;
  std::string error;
  if (!ReadRequest(&request)) {
    ClearSensitiveString(&request);
    std::cerr << "invalid webview request: cannot read bounded stdin request\n";
    return EXIT_FAILURE;
  }
  if (!mocktail::runtime::DecodeWebViewRequest(request, &state.initial_url,
                                               &error)) {
    ClearSensitiveString(&request);
    std::cerr << "invalid webview request: " << error << '\n';
    return EXIT_FAILURE;
  }
  ClearSensitiveString(&request);
  if (!PrepareControlChannel()) {
    ClearSensitiveString(&state.initial_url);
    std::cerr << "invalid webview control channel\n";
    return EXIT_FAILURE;
  }
  g_weak_ref_init(&state.primary_web_view, nullptr);
  g_weak_ref_init(&state.primary_window, nullptr);

  GtkApplication *application = gtk_application_new(
      "org.mocktail.WebViewHelper", G_APPLICATION_NON_UNIQUE);
  state.application = application;
  g_signal_connect(application, "activate", G_CALLBACK(Activate), &state);
  state.termination_source =
      g_unix_signal_add(SIGTERM, OnTerminationSignal, &state);
  state.control_source = g_unix_fd_add(
      mocktail::runtime::kWebViewHelperControlDescriptor,
      static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL),
      OnControlChannel, &state);
  int status = EXIT_FAILURE;
  if (state.control_source == 0) {
    state.startup_failed = true;
  } else {
    status = g_application_run(G_APPLICATION(application), argc, argv);
  }
  if (state.control_source != 0) {
    g_source_remove(state.control_source);
    state.control_source = 0;
  }
  if (state.hybrid_write_source != 0) {
    g_source_remove(state.hybrid_write_source);
    state.hybrid_write_source = 0;
  }
  if (state.termination_source != 0) {
    g_source_remove(state.termination_source);
    state.termination_source = 0;
  }
  g_object_unref(application);
  if (state.network_session != nullptr) {
    g_object_unref(state.network_session);
  }
  g_weak_ref_clear(&state.primary_window);
  g_weak_ref_clear(&state.primary_web_view);
  close(mocktail::runtime::kWebViewHelperControlDescriptor);
  ClearSensitiveString(&state.initial_url);
  ClearSensitiveString(&state.pending_navigation_url);
  ClearSensitiveString(&state.pending_roblox_cookie);
  for (std::string& packet : state.pending_hybrid_packets) {
    ClearSensitiveString(&packet);
  }
  return state.startup_failed ? EXIT_FAILURE : status;
}
