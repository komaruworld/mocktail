#include "runtime/roblox_permissions_bridge.h"

#include <nlohmann/json.hpp>

namespace mocktail {
namespace runtime {
namespace {
constexpr std::size_t kMaximumJsonBytes = 64 * 1024;
constexpr std::size_t kMaximumFeatures = 64;
constexpr char kMicrophone[] = "MICROPHONE_ACCESS";
constexpr char kLocalNetwork[] = "LOCAL_NETWORK";

struct Response {
  nlohmann::json body;
  jint code = 0;
  const char* outcome = "DENIED";
};

bool HasBoundedDepth(const std::string& json) {
  int depth = 0;
  bool quoted = false;
  bool escaped = false;
  for (const char ch : json) {
    if (quoted) {
      if (escaped)
        escaped = false;
      else if (ch == '\\')
        escaped = true;
      else if (ch == '"')
        quoted = false;
    } else if (ch == '"') {
      quoted = true;
    } else if (ch == '{' || ch == '[') {
      if (++depth > 8)
        return false;
    } else if (ch == '}' || ch == ']') {
      --depth;
    }
  }
  return true;  // Syntax is checked by the JSON parser below.
}

Response Answer(JNIEnv* env, jstring message, std::size_t method,
                bool microphone_enabled) {
  const bool support = method == 0;
  const bool upsell = method >= 3;
  Response rejected{
      support  ? nlohmann::json{{"permissions", nlohmann::json::array()}}
      : upsell ? nlohmann::json{{"upsellStatus", "HIDE"},
                                {"hiddenUpsellPermissions", {kMicrophone}}}
               : nlohmann::json{{"status", "DENIED"},
                                {"missingPermissions", {kMicrophone}}},
      13, "invalid_request"};
  if (!message)
    return rejected;
  const jsize size = env->GetStringUTFLength(message);
  if (size <= 0 || static_cast<std::size_t>(size) > kMaximumJsonBytes) {
    return rejected;
  }
  const char* bytes = env->GetStringUTFChars(message, nullptr);
  if (!bytes)
    return rejected;
  const std::string owned(bytes, static_cast<std::size_t>(size));
  env->ReleaseStringUTFChars(message, bytes);
  if (!HasBoundedDepth(owned))
    return rejected;
  const auto request = nlohmann::json::parse(owned, nullptr, false);
  if (!request.is_object())
    return rejected;
  if (support) {
    return {{{"permissions", {kMicrophone, kLocalNetwork}}}, 0, "supported"};
  }
  const auto features = request.find("permissions");
  if (features == request.end() || !features->is_array() ||
      features->size() > kMaximumFeatures) {
    return rejected;
  }
  auto missing = nlohmann::json::array();
  for (const auto& feature : *features) {
    if (!feature.is_string())
      return rejected;
    const auto& name = feature.get_ref<const std::string&>();
    if (name.empty() || name.size() > 128 ||
        name.find_first_of("\r\n\t") != std::string::npos ||
        name.find('\0') != std::string::npos) {
      return rejected;
    }
    // The APK maps LOCAL_NETWORK to no Android runtime permissions. The
    // existing host network implementation remains subject to OS policy.
    if (name != kLocalNetwork && (name != kMicrophone || !microphone_enabled)) {
      missing.push_back(name);
    }
  }
  if (upsell) {
    // The host has no Android runtime-permission dialog. Do not invite a UI
    // request that cannot grant access, especially for camera/contacts.
    return {{{"upsellStatus", "HIDE"}, {"hiddenUpsellPermissions", *features}},
            0,
            "HIDE"};
  }
  const char* status = missing.empty() ? "AUTHORIZED" : "DENIED";
  return {{{"status", status}, {"missingPermissions", std::move(missing)}},
          0,
          status};
}

}  // namespace

RobloxPermissionsBridge::RobloxPermissionsBridge(
    JniEnvironmentProvider environment,
    RobloxPermissionsMessageBusSymbols symbols,
    RobloxPermissionsMessageBusObjects objects, bool microphone_enabled)
    : bridge_(std::make_unique<RobloxMessageBusRequestBridge>(
          environment, symbols, objects, "PermissionsProtocol",
          std::vector<std::string>{"SupportsPermissions", "HasPermissions",
                                   "PermissionsRequest",
                                   "ShouldShowPermissionUpsell",
                                   "ShouldShowRequestPermissionRationale"},
          [microphone_enabled](JNIEnv* env, jstring message,
                               std::size_t method) {
            auto response = Answer(env, message, method, microphone_enabled);
            return RobloxMessageBusReply{response.body.dump(), response.code,
                                         response.outcome};
          })) {}

RobloxPermissionsBridge::~RobloxPermissionsBridge() = default;

Status RobloxPermissionsBridge::Initialize() {
  return bridge_->Initialize();
}

Status RobloxPermissionsBridge::Shutdown() {
  return bridge_->Shutdown();
}
}  // namespace runtime
}  // namespace mocktail
