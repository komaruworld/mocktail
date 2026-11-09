#include "runtime/roblox_message_bus_request_bridge.h"

#include <condition_variable>
#include <cstdio>
#include <utility>

namespace mocktail {
namespace runtime {
namespace {
Status Unavailable(const char* message) {
  return Status::Error(StatusCode::kUnavailable, message);
}

Status CheckJni(JNIEnv* env) {
  if (!env->ExceptionCheck())
    return Status::Ok();
  env->ExceptionClear();
  return Unavailable("MessageBus JNI operation failed");
}

// JNI local references are never retained across callback invocations.
struct LocalString {
  JNIEnv* env;
  jstring value;

  LocalString(JNIEnv* source, const char* text)
      : env(source), value(source->NewStringUTF(text)) {}

  ~LocalString() {
    if (value)
      env->DeleteLocalRef(value);
  }
};

}  // namespace

struct RobloxMessageBusRequestBridge::State {
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

  RobloxMessageBusSymbols symbols;
  jobject bus = nullptr;
  jstring protocol = nullptr;
  std::string protocol_name;
  std::vector<std::string> methods;
  Handler handler;
  std::vector<Registration> registrations;
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
      if (!state->accepting)
        return;
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

    const auto response = state->handler(env, message, endpoint->method);
    // Request bodies and correlation IDs must never be logged.
    std::fprintf(stderr, "  [messagebus] %s method=%s result=%s transport=%s\n",
                 state->protocol_name.c_str(),
                 state->methods[endpoint->method].c_str(), response.outcome,
                 id ? "async" : "legacy");
    LocalString body(env, response.json.c_str());
    LocalString telemetry(env, "{}");
    LocalString method(env, state->methods[endpoint->method].c_str());
    const Status allocated = CheckJni(env);
    if (!body.value || !telemetry.value || !method.value || !allocated.ok()) {
      std::fprintf(stderr, "  [messagebus] response allocation failed\n");
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
      std::fprintf(stderr, "  [messagebus] response delivery failed\n");
    }
  }
};

RobloxMessageBusRequestBridge::RobloxMessageBusRequestBridge(
    JniEnvironmentProvider environment, RobloxMessageBusSymbols symbols,
    RobloxMessageBusObjects objects, std::string protocol,
    std::vector<std::string> methods, Handler handler)
    : environment_(environment),
      symbols_(symbols),
      objects_(objects),
      protocol_name_(std::move(protocol)),
      methods_(std::move(methods)),
      handler_(std::move(handler)) {}

RobloxMessageBusRequestBridge::~RobloxMessageBusRequestBridge() {
  (void)Shutdown();
}

Status RobloxMessageBusRequestBridge::Initialize() {
  std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
  if (state_) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "MessageBus request bridge is already initialized");
  }
  if (!environment_.valid() || !symbols_.complete() || !objects_.complete() ||
      protocol_name_.empty() || methods_.empty() || methods_.size() > 64 ||
      !handler_) {
    return Unavailable(
        "MessageBus request bridge prerequisites are incomplete");
  }
  JNIEnv* env = nullptr;
  Status status = environment_.Acquire(&env);
  if (!status.ok())
    return status;
  state_ = std::make_shared<State>();
  state_->symbols = symbols_;
  state_->protocol_name = protocol_name_;
  state_->methods = methods_;
  state_->handler = handler_;
  state_->registrations.resize(methods_.size());
  state_->bus = env->NewGlobalRef(objects_.message_bus);
  LocalString protocol(env, protocol_name_.c_str());
  state_->protocol = static_cast<jstring>(env->NewGlobalRef(protocol.value));
  auto fail = [this](Status failure) {
    (void)ShutdownLocked();
    return failure;
  };
  if (!state_->bus || !state_->protocol || !CheckJni(env).ok()) {
    return fail(
        Unavailable("could not retain MessageBus request bridge roots"));
  }
  for (std::size_t i = 0; i < methods_.size(); ++i) {
    auto& entry = state_->registrations[i];
    auto endpoint =
        std::make_shared<State::Endpoint>(State::Endpoint{state_, i});
    LocalString method(env, methods_[i].c_str());
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
          Unavailable("could not create MessageBus request bridge callbacks"));
    }
    jobject connection =
        symbols_.subscribe_request(env, state_->bus, state_->protocol,
                                   entry.method, entry.callback, JNI_FALSE);
    if (connection) {
      jclass cls = env->GetObjectClass(connection);
      jfieldID pointer = cls ? env->GetFieldID(cls, "a", "J") : nullptr;
      if (pointer)
        entry.connection_handle = env->GetLongField(connection, pointer);
      if (cls)
        env->DeleteLocalRef(cls);
      entry.connection = env->NewGlobalRef(connection);
      if (!entry.connection && entry.connection_handle) {
        symbols_.delete_connection(env, connection, entry.connection_handle);
        entry.connection_handle = 0;
      }
      env->DeleteLocalRef(connection);
    }
    if (!entry.connection || !entry.connection_handle || !CheckJni(env).ok()) {
      return fail(
          Unavailable("could not subscribe MessageBus request bridge request"));
    }
    entry.handler_registered = true;
    symbols_.set_async_handler(env, state_->bus, state_->protocol, entry.method,
                               entry.handler);
    status = CheckJni(env);
    if (!status.ok())
      return fail(status);
  }
  std::fprintf(stderr, "  [messagebus] %s ready legacy=%zu async=%zu\n",
               protocol_name_.c_str(), methods_.size(), methods_.size());
  return Status::Ok();
}

Status RobloxMessageBusRequestBridge::Shutdown() {
  std::lock_guard<std::mutex> lifecycle(lifecycle_mutex_);
  return ShutdownLocked();
}

Status RobloxMessageBusRequestBridge::ShutdownLocked() {
  if (!state_)
    return Status::Ok();
  {
    std::unique_lock<std::mutex> lock(state_->mutex);
    state_->accepting = false;
    state_->drained.wait(lock, [this] {
      return state_->in_flight == 0;
    });
  }
  JNIEnv* env = nullptr;
  Status status = environment_.Acquire(&env);
  if (!status.ok())
    return status;  // Keep roots for a later shutdown retry.
  // Clear any exception from partially failed initialization before cleanup.
  const Status pending = CheckJni(env);
  if (!pending.ok())
    status = pending;
  for (auto& entry : state_->registrations) {
    if (entry.handler_registered) {
      symbols_.clear_handler(env, state_->bus, state_->protocol, entry.method);
      const Status cleared = CheckJni(env);
      if (!cleared.ok())
        status = cleared;
      entry.handler_registered = false;
    }
    if (entry.connection && entry.connection_handle) {
      symbols_.delete_connection(env, entry.connection,
                                 entry.connection_handle);
      const Status disconnected = CheckJni(env);
      if (!disconnected.ok())
        status = disconnected;
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
    if (entry.connection)
      env->DeleteGlobalRef(entry.connection);
    if (entry.method)
      env->DeleteGlobalRef(entry.method);
    entry = {};
    const Status cleared = CheckJni(env);
    if (!cleared.ok())
      status = cleared;
  }
  if (state_->protocol)
    env->DeleteGlobalRef(state_->protocol);
  if (state_->bus)
    env->DeleteGlobalRef(state_->bus);
  state_.reset();
  return status;
}

}  // namespace runtime
}  // namespace mocktail
