#include "runtime/roblox_permissions_bridge.h"

#include <array>
#include <condition_variable>
#include <cstdio>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace mocktail {
namespace runtime {
namespace {

constexpr char kProtocol[] = "PermissionsProtocol";
constexpr char kMicrophone[] = "MICROPHONE_ACCESS";
constexpr char kLocalNetwork[] = "LOCAL_NETWORK";
constexpr const char* kMethods[] = {
    "SupportsPermissions", "HasPermissions", "PermissionsRequest",
    "ShouldShowPermissionUpsell", "ShouldShowRequestPermissionRationale"};
constexpr std::size_t kMethodCount = std::size(kMethods);
constexpr std::size_t kMaximumJsonBytes = 64 * 1024;
constexpr std::size_t kMaximumFeatures = 64;

Status Unavailable(const char* message) {
  return Status::Error(StatusCode::kUnavailable, message);
}

Status CheckJni(JNIEnv* env) {
  if (!env->ExceptionCheck()) return Status::Ok();
  env->ExceptionClear();
  return Unavailable("PermissionsProtocol JNI operation failed");
}

// JNI local references are never retained across callback invocations.
struct LocalString {
  JNIEnv* env;
  jstring value;
  LocalString(JNIEnv* source, const char* text)
      : env(source), value(source->NewStringUTF(text)) {}
  ~LocalString() {
    if (value) env->DeleteLocalRef(value);
  }
};

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
      if (++depth > 8) return false;
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
  if (!message) return rejected;
  const jsize size = env->GetStringUTFLength(message);
  if (size <= 0 || static_cast<std::size_t>(size) > kMaximumJsonBytes) {
    return rejected;
  }
  const char* bytes = env->GetStringUTFChars(message, nullptr);
  if (!bytes) return rejected;
  const std::string owned(bytes, static_cast<std::size_t>(size));
  env->ReleaseStringUTFChars(message, bytes);
  if (!HasBoundedDepth(owned)) return rejected;
  const auto request = nlohmann::json::parse(owned, nullptr, false);
  if (!request.is_object()) return rejected;
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
    if (!feature.is_string()) return rejected;
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

struct RobloxPermissionsBridge::State {
  struct Endpoint {
    std::shared_ptr<State> state;
    std::size_t method;
  };
  struct Registration {
    jobject callback = nullptr;
    jobject handler = nullptr;
    jobject connection = nullptr;
    jlong connection_handle = 0;
    jstring method = nullptr;
    bool handler_registered = false;
  };

  RobloxPermissionsMessageBusSymbols symbols;
  jobject bus = nullptr;
  jstring protocol = nullptr;
  bool microphone_enabled = false;
  std::array<Registration, kMethodCount> registrations;
  std::mutex mutex;
  std::condition_variable drained;
  bool accepting = true;
  std::size_t in_flight = 0;

  static void Legacy(void* context, JNIEnv* env, jstring message) {
    Dispatch(context, env, message, nullptr);
  }
  static void Async(void* context, JNIEnv* env, jstring message, jstring id) {
    Dispatch(context, env, message, id);
  }
  static void Dispatch(void* context, JNIEnv* env, jstring message,
                       jstring id) {
    const auto* endpoint = static_cast<Endpoint*>(context);
    const auto state = endpoint->state;
    {
      std::lock_guard<std::mutex> lock(state->mutex);
      if (!state->accepting) return;
      ++state->in_flight;
    }
    struct Finish {
      std::shared_ptr<State> state;
      ~Finish() {
        std::lock_guard<std::mutex> lock(state->mutex);
        --state->in_flight;
        state->drained.notify_all();
      }
    } finish{state};

    Response response =
        Answer(env, message, endpoint->method, state->microphone_enabled);
    // No payload, feature strings, or correlation IDs in logs.
    std::fprintf(stderr,
                 "  [permissions] %s method=%s result=%s microphone=%s "
                 "transport=%s\n",
                 kProtocol, kMethods[endpoint->method], response.outcome,
                 state->microphone_enabled ? "enabled" : "disabled",
                 id ? "async" : "legacy");
    LocalString body(env, response.body.dump().c_str());
    LocalString telemetry(env, "{}");
    LocalString method(env, kMethods[endpoint->method]);
    const Status allocated = CheckJni(env);
    if (!body.value || !telemetry.value || !method.value || !allocated.ok()) {
      std::fprintf(stderr, "  [permissions] response allocation failed\n");
      return;
    }
    // The APK publishes the legacy response even for the newer async API,
    // then resolves the individual request by its opaque response ID.
    state->symbols.publish_response(env, state->bus, state->protocol,
                                    method.value, body.value, response.code,
                                    telemetry.value);
    const Status published = CheckJni(env);
    if (id) {
      state->symbols.call_response_handler(env, state->bus, id, body.value);
    }
    if (!CheckJni(env).ok() || !published.ok()) {
      std::fprintf(stderr, "  [permissions] response delivery failed\n");
    }
  }
};

RobloxPermissionsBridge::RobloxPermissionsBridge(
    JniEnvironmentProvider environment,
    RobloxPermissionsMessageBusSymbols symbols,
    RobloxPermissionsMessageBusObjects objects, bool microphone_enabled)
    : environment_(environment),
      symbols_(symbols),
      objects_(objects),
      microphone_enabled_(microphone_enabled) {}

RobloxPermissionsBridge::~RobloxPermissionsBridge() { (void)Shutdown(); }

Status RobloxPermissionsBridge::Initialize() {
  std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
  if (state_) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "PermissionsProtocol is already initialized");
  }
  if (!environment_.valid() || !symbols_.complete() || !objects_.complete()) {
    return Unavailable("PermissionsProtocol prerequisites are incomplete");
  }
  JNIEnv* env = nullptr;
  Status status = environment_.Acquire(&env);
  if (!status.ok()) return status;
  state_ = std::make_shared<State>();
  state_->symbols = symbols_;
  state_->microphone_enabled = microphone_enabled_;
  state_->bus = env->NewGlobalRef(objects_.message_bus);
  LocalString protocol(env, kProtocol);
  state_->protocol = static_cast<jstring>(env->NewGlobalRef(protocol.value));
  auto fail = [this](Status failure) {
    (void)ShutdownLocked();
    return failure;
  };
  if (!state_->bus || !state_->protocol || !CheckJni(env).ok()) {
    return fail(Unavailable("could not retain PermissionsProtocol roots"));
  }
  for (std::size_t i = 0; i < kMethodCount; ++i) {
    auto& entry = state_->registrations[i];
    auto endpoint =
        std::make_shared<State::Endpoint>(State::Endpoint{state_, i});
    LocalString method(env, kMethods[i]);
    entry.method = static_cast<jstring>(env->NewGlobalRef(method.value));
    jobject callback = objects_.create_raw_callback(objects_.context, endpoint,
                                                    &State::Legacy);
    if (callback) {
      entry.callback = env->NewGlobalRef(callback);
      if (!entry.callback)
        objects_.clear_raw_callback(objects_.context, callback);
      env->DeleteLocalRef(callback);
    }
    jobject handler = objects_.create_async_handler(objects_.context, endpoint,
                                                    &State::Async);
    if (handler) {
      entry.handler = env->NewGlobalRef(handler);
      if (!entry.handler)
        objects_.clear_async_handler(objects_.context, handler);
      env->DeleteLocalRef(handler);
    }
    if (!entry.method || !entry.callback || !entry.handler ||
        !CheckJni(env).ok()) {
      return fail(
          Unavailable("could not create PermissionsProtocol callbacks"));
    }
    jobject connection =
        symbols_.subscribe_request(env, state_->bus, state_->protocol,
                                   entry.method, entry.callback, JNI_FALSE);
    if (connection) {
      jclass cls = env->GetObjectClass(connection);
      jfieldID pointer = cls ? env->GetFieldID(cls, "a", "J") : nullptr;
      if (pointer)
        entry.connection_handle = env->GetLongField(connection, pointer);
      if (cls) env->DeleteLocalRef(cls);
      entry.connection = env->NewGlobalRef(connection);
      if (!entry.connection && entry.connection_handle) {
        symbols_.delete_connection(env, connection, entry.connection_handle);
        entry.connection_handle = 0;
      }
      env->DeleteLocalRef(connection);
    }
    if (!entry.connection || !entry.connection_handle || !CheckJni(env).ok()) {
      return fail(
          Unavailable("could not subscribe PermissionsProtocol request"));
    }
    entry.handler_registered = true;
    symbols_.set_async_handler(env, state_->bus, state_->protocol, entry.method,
                               entry.handler);
    status = CheckJni(env);
    if (!status.ok()) return fail(status);
  }
  std::fprintf(stderr,
               "  [permissions] PermissionsProtocol ready microphone=%s "
               "legacy=5 async=5\n",
               microphone_enabled_ ? "enabled" : "disabled");
  return Status::Ok();
}

Status RobloxPermissionsBridge::Shutdown() {
  std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
  return ShutdownLocked();
}

Status RobloxPermissionsBridge::ShutdownLocked() {
  if (!state_) return Status::Ok();
  {
    std::unique_lock<std::mutex> lock(state_->mutex);
    state_->accepting = false;
    state_->drained.wait(lock, [this] { return state_->in_flight == 0; });
  }
  JNIEnv* env = nullptr;
  Status status = environment_.Acquire(&env);
  if (!status.ok()) return status;  // Keep roots for a later shutdown retry.
  // Clear any exception from partially failed initialization before cleanup.
  const Status pending = CheckJni(env);
  if (!pending.ok()) status = pending;
  for (auto& entry : state_->registrations) {
    if (entry.handler_registered) {
      symbols_.clear_handler(env, state_->bus, state_->protocol, entry.method);
      const Status cleared = CheckJni(env);
      if (!cleared.ok()) status = cleared;
      entry.handler_registered = false;
    }
    if (entry.connection && entry.connection_handle) {
      symbols_.delete_connection(env, entry.connection,
                                 entry.connection_handle);
      const Status disconnected = CheckJni(env);
      if (!disconnected.ok()) status = disconnected;
      entry.connection_handle = 0;
    }
    if (entry.callback) {
      objects_.clear_raw_callback(objects_.context, entry.callback);
      env->DeleteGlobalRef(entry.callback);
    }
    if (entry.handler) {
      objects_.clear_async_handler(objects_.context, entry.handler);
      env->DeleteGlobalRef(entry.handler);
    }
    if (entry.connection) env->DeleteGlobalRef(entry.connection);
    if (entry.method) env->DeleteGlobalRef(entry.method);
    entry = {};
    const Status cleared = CheckJni(env);
    if (!cleared.ok()) status = cleared;
  }
  if (state_->protocol) env->DeleteGlobalRef(state_->protocol);
  if (state_->bus) env->DeleteGlobalRef(state_->bus);
  state_.reset();
  return status;
}

}  // namespace runtime
}  // namespace mocktail
