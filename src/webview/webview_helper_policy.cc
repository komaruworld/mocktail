#include "webview/webview_helper_policy.h"

#include <glib.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace mocktail {
namespace webview {
namespace {

constexpr std::size_t kMaximumHybridIdentifierBytes = 128;
constexpr std::size_t kMaximumCallbackIdentifierBytes = 1024;
constexpr char kFallbackRobloxVersion[] = "2.725.1142";
constexpr char kBridgeSource[] = R"JS(
(() => {
  "use strict";
  const handlers = window.webkit && window.webkit.messageHandlers;
  const handler = handlers && handlers.executeRoblox;
  if (!handler) {
    return;
  }
  const bridge = {};
  Object.defineProperty(bridge, "executeRoblox", {
    value: (query) => handler.postMessage(JSON.parse(query)),
    enumerable: true,
    writable: false,
    configurable: false
  });
  try {
    Object.defineProperty(window, "__globalRobloxAndroidBridge__", {
      value: bridge,
      enumerable: true,
      writable: false,
      configurable: false
    });
  } catch (_) {
    // A page-defined object is never allowed to replace the native bridge.
  }
})();
)JS";

bool IsSafeVersion(std::string_view version) {
  return !version.empty() && version.size() <= 32 &&
         std::all_of(version.begin(), version.end(), [](unsigned char value) {
           return (value >= '0' && value <= '9') || value == '.' ||
                  value == '-';
         });
}

bool ReadBoundedString(const nlohmann::json& object, const char* key,
                       std::size_t maximum_bytes, std::string* value,
                       bool optional = false) {
  const auto found = object.find(key);
  if (found == object.end()) {
    if (optional) {
      value->clear();
      return true;
    }
    return false;
  }
  if (!found->is_string()) {
    return false;
  }
  const auto& text = found->get_ref<const std::string&>();
  if (text.size() > maximum_bytes ||
      std::any_of(text.begin(), text.end(), [](unsigned char character) {
        return character < 0x20 || character == 0x7f;
      })) {
    return false;
  }
  *value = text;
  return true;
}

bool ExtractBoundedJscString(JSCValue* value, std::string* text) {
  if (value == nullptr || text == nullptr || !jsc_value_is_string(value)) {
    return false;
  }
  GBytes* bytes = jsc_value_to_string_as_bytes(value);
  if (bytes == nullptr) {
    return false;
  }
  gsize size = 0;
  const auto* data = static_cast<const char*>(g_bytes_get_data(bytes, &size));
  const bool valid =
      data != nullptr && size != 0 && size <= kMaximumHybridCommandBytes;
  if (valid) {
    text->assign(data, size);
  }
  g_bytes_unref(bytes);
  return valid;
}

bool IsRobloxHost(const char* host) {
  if (host == nullptr) {
    return false;
  }
  constexpr std::string_view kRoot = "roblox.com";
  const std::string_view value(host);
  if (value.size() < kRoot.size()) {
    return false;
  }
  const std::string_view suffix = value.substr(value.size() - kRoot.size());
  if (g_ascii_strcasecmp(std::string(suffix).c_str(),
                         std::string(kRoot).c_str()) != 0) {
    return false;
  }
  return value.size() == kRoot.size() ||
         value[value.size() - kRoot.size() - 1] == '.';
}

}  // namespace

const char* AndroidBridgeSource() { return kBridgeSource; }

std::string BuildRobloxAndroidUserAgent() {
  const char* configured_version = std::getenv("MOCKTAIL_ROBLOX_VERSION");
  const std::string_view version =
      configured_version != nullptr && IsSafeVersion(configured_version)
          ? std::string_view(configured_version)
          : std::string_view(kFallbackRobloxVersion);
  return "Mozilla/5.0 AppleWebKit/605.1.15 (KHTML, like Gecko)  ROBLOX "
         "Android App " +
         std::string(version) + " Tablet Hybrid()  GooglePlayStore RobloxApp/" +
         std::string(version) + "(GlobalDist; GooglePlayStore)";
}

std::string BoundedLogToken(const char* value, std::string_view fallback) {
  if (value == nullptr || *value == '\0') {
    return std::string(fallback);
  }
  constexpr std::size_t kMaximumLogTokenBytes = 128;
  std::string token;
  for (const unsigned char character : std::string_view(value)) {
    if (token.size() == kMaximumLogTokenBytes) {
      break;
    }
    token.push_back(character >= 0x21 && character <= 0x7e
                        ? static_cast<char>(character)
                        : '?');
  }
  return token;
}

UriPolicyResult EvaluateNavigationUri(const char* uri) {
  UriPolicyResult result;
  if (uri == nullptr) {
    return result;
  }
  if (std::string_view(uri) == "about:blank") {
    result.allowed = true;
    result.scheme = "about";
    result.host = "none";
    return result;
  }

  GError* parse_error = nullptr;
  GUri* parsed = g_uri_parse(uri, G_URI_FLAGS_NONE, &parse_error);
  if (parse_error != nullptr) {
    g_error_free(parse_error);
  }
  if (parsed == nullptr) {
    return result;
  }

  const char* scheme = g_uri_get_scheme(parsed);
  const char* host = g_uri_get_host(parsed);
  result.scheme = BoundedLogToken(scheme, "invalid");
  result.host = BoundedLogToken(host, "none");
  const int port = g_uri_get_port(parsed);
  result.allowed =
      scheme != nullptr && g_ascii_strcasecmp(scheme, "https") == 0 &&
      host != nullptr && *host != '\0' &&
      g_uri_get_userinfo(parsed) == nullptr && (port == -1 || port == 443);
  result.privileged_bridge_allowed = result.allowed && IsRobloxHost(host);
  g_uri_unref(parsed);
  return result;
}

const char* CaptchaEventName(CaptchaEventType type) {
  return type == CaptchaEventType::kSuccess ? "CaptchaSuccess" : "CaptchaShown";
}

bool ExtractExecuteRobloxCommand(JSCValue* value, std::string* command) {
  if (value == nullptr || command == nullptr || !jsc_value_is_object(value)) {
    return false;
  }
  char* serialized = jsc_value_to_json(value, 0);
  if (serialized == nullptr) {
    return false;
  }
  const std::size_t size = strnlen(serialized, kMaximumHybridCommandBytes + 1);
  const bool valid = size != 0 && size <= kMaximumHybridCommandBytes;
  if (valid) {
    command->assign(serialized, size);
  }
  g_free(serialized);
  return valid;
}

bool ExtractRobloxWkHybridCommand(JSCValue* value, std::string* command) {
  if (value == nullptr || command == nullptr || !jsc_value_is_object(value)) {
    return false;
  }
  JSCValue* property = jsc_value_object_get_property(value, "command");
  const bool extracted = ExtractBoundedJscString(property, command);
  if (property != nullptr) {
    g_object_unref(property);
  }
  return extracted;
}

bool ParseCaptchaEvent(std::string_view command, CaptchaEvent* event) {
  if (event == nullptr || command.empty() ||
      command.size() > kMaximumHybridCommandBytes) {
    return false;
  }
  const nlohmann::json message = nlohmann::json::parse(
      command.begin(), command.end(), nullptr, false, false);
  if (message.is_discarded() || !message.is_object()) {
    return false;
  }

  std::string module;
  std::string function;
  if (!ReadBoundedString(message, "moduleID", kMaximumHybridIdentifierBytes,
                         &module) ||
      !ReadBoundedString(message, "functionName", kMaximumHybridIdentifierBytes,
                         &function) ||
      module != "Navigation" || function != "navigateToFeature") {
    return false;
  }
  const auto parameters = message.find("params");
  if (parameters == message.end() || !parameters->is_object()) {
    return false;
  }
  const auto navigation = parameters->find("params");
  if (navigation == parameters->end() || !navigation->is_object()) {
    return false;
  }

  std::string feature;
  if (!ReadBoundedString(*navigation, "feature", kMaximumHybridIdentifierBytes,
                         &feature) ||
      !ReadBoundedString(message, "callbackID", kMaximumCallbackIdentifierBytes,
                         &event->callback_id, true)) {
    return false;
  }
  if (feature == "CaptchaShown") {
    event->type = CaptchaEventType::kShown;
    return true;
  }
  if (feature == "CaptchaSuccess") {
    event->type = CaptchaEventType::kSuccess;
    return true;
  }
  return false;
}

std::string BuildCallbackScript(std::string_view callback_id) {
  const std::string quoted_callback = nlohmann::json(callback_id).dump();
  return "(() => { try { if (window.Roblox.Hybrid && "
         "window.Roblox.Hybrid.Bridge.nativeCallback && typeof "
         "window.Roblox.Hybrid.Bridge.nativeCallback === \"function\") { "
         "window.Roblox.Hybrid.Bridge.nativeCallback(" +
         quoted_callback +
         ", true, {}); return true; } } catch (_) {} return false; })();";
}

}  // namespace webview
}  // namespace mocktail
