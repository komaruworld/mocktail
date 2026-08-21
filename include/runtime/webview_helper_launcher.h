#ifndef MOCKTAIL_RUNTIME_WEBVIEW_HELPER_LAUNCHER_H_
#define MOCKTAIL_RUNTIME_WEBVIEW_HELPER_LAUNCHER_H_

#include <sys/types.h>

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mocktail {
namespace runtime {

inline constexpr std::size_t kMaximumWebViewUrlBytes = 16 * 1024;
inline constexpr std::size_t kMaximumWebViewTitleBytes = 512;
inline constexpr std::size_t kMaximumWebViewJavaScriptBytes = 64 * 1024;
inline constexpr std::size_t kMaximumWebViewCookieBytes = 16 * 1024;
inline constexpr std::size_t kMaximumWebViewControlPacketBytes =
    kMaximumWebViewJavaScriptBytes + 12;
inline constexpr std::size_t kMaximumWebViewHybridEventBytes = 64 * 1024;
inline constexpr std::size_t kMaximumWebViewEventPacketBytes =
    kMaximumWebViewHybridEventBytes + 12;

// The launcher installs one end of a private SOCK_SEQPACKET socket at this
// descriptor in the helper. It is never exposed through argv or the
// environment and is closed automatically when either process exits.
inline constexpr int kWebViewHelperControlDescriptor = 198;

enum class WebViewHelperControlOperation : unsigned char {
  kLoadUrl = 1,
  kSetTitle = 2,
  kSetVisible = 3,
  kEvaluateJavaScript = 4,
  kClose = 5,
  kSetBackNavigationDisabled = 6,
  kSetShowDomainAsTitle = 7,
  kSetRobloxCookie = 8,
  kClearRobloxCookie = 9,
  kRetainPersistentRobloxCookie = 10,
};

struct WebViewHelperControlCommand {
  WebViewHelperControlOperation operation =
      WebViewHelperControlOperation::kClose;
  std::string payload;
  bool visible = false;
};

enum class WebViewHelperEventType : unsigned char {
  kExecuteRoblox = 1,
  kRobloxWkHybrid = 2,
  kReady = 3,
};

struct WebViewHelperEvent {
  WebViewHelperEventType type = WebViewHelperEventType::kExecuteRoblox;
  std::string payload;
};

bool ValidateWebViewUrl(std::string_view url, std::string *error = nullptr);
bool NormalizeWebViewUrl(std::string_view url, std::string *normalized_url,
                         std::string *error = nullptr);
bool DecodeWebViewRequest(std::string_view request, std::string *url,
                          std::string *error = nullptr);
bool DecodeWebViewHelperControlPacket(std::string_view packet,
                                      WebViewHelperControlCommand* command,
                                      std::string* error = nullptr);
bool EncodeWebViewHelperEventPacket(WebViewHelperEventType type,
                                    std::string_view payload,
                                    std::string* packet);
bool DecodeWebViewHelperEventPacket(std::string_view packet,
                                    WebViewHelperEvent* event,
                                    std::string* error = nullptr);

struct WebViewHelperExitObserver;
struct WebViewHelperLaunchResult;

class WebViewHelperProcess final {
 public:
  struct State;

  bool running() const;
  pid_t process_id() const;
  bool LoadUrl(std::string_view url) const;
  bool SetTitle(std::string_view title) const;
  bool SetVisible(bool visible) const;
  bool EvaluateJavaScript(std::string_view script) const;
  bool SetBackNavigationDisabled(bool disabled) const;
  bool SetShowDomainAsTitle(bool enabled) const;
  // A non-empty value replaces the WebKit credential for the current runtime.
  // An empty value leaves WebKit's persistent cookie store authoritative so a
  // browser login survives a process restart.
  bool SetRobloxCookie(std::string_view value) const;
  bool ClearRobloxCookie() const;
  bool WaitUntilReady(std::chrono::milliseconds timeout) const;
  bool DrainEvents(std::vector<WebViewHelperEvent>* events) const;
  bool RequestClose() const;

 private:
  explicit WebViewHelperProcess(std::shared_ptr<State> state);

  std::shared_ptr<State> state_;

  friend struct WebViewHelperLaunchResult;
  friend WebViewHelperLaunchResult LaunchWebViewHelper(
      const std::filesystem::path&, std::string_view,
      WebViewHelperExitObserver);
};

struct WebViewHelperLaunchResult {
  bool spawned = false;
  pid_t process_id = -1;
  std::shared_ptr<WebViewHelperProcess> process;
  std::string error;

  explicit operator bool() const { return spawned && error.empty(); }
};

// Child-exit observer owned by the detached reaper. The callback must not call
// JNI or retain raw process-lifetime pointers; it may only record completion
// into the shared context for a main-thread drain.
struct WebViewHelperExitObserver {
  std::shared_ptr<void> context;
  void (*on_exit)(void *context) = nullptr;

  bool valid() const { return context != nullptr && on_exit != nullptr; }
};

// Starts a standalone helper with an empty argument list beyond argv[0]. The
// bounded request is supplied on stdin so login URLs do not appear in the
// process command line. Later mutations use a private bounded packet socket.
// Internal Roblox www: routes are normalized to the fixed Roblox HTTPS origin
// before the helper receives them. A detached reaper owns the child after a
// successful return; this call does not wait for the helper window to close.
WebViewHelperLaunchResult LaunchWebViewHelper(
    const std::filesystem::path& helper, std::string_view url,
    WebViewHelperExitObserver exit_observer = {});

} // namespace runtime
} // namespace mocktail

#endif // MOCKTAIL_RUNTIME_WEBVIEW_HELPER_LAUNCHER_H_
