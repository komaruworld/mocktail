#include "runtime/roblox_browser_service_bridge.h"

#include <cstdio>
#include <iterator>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace mocktail {
namespace runtime {
namespace {

constexpr char kMemStorageClass[] =
    "com/roblox/engine/jni/memstorage/MemStorage";
constexpr char kMemStorageConnectionClass[] =
    "com/roblox/engine/jni/memstorage/Connection";
constexpr std::size_t kMaximumBrowserWindowTitleBytes = 512;

Status Invalid(std::string message) {
  return Status::Error(StatusCode::kInvalidArgument, std::move(message));
}

Status Unavailable(std::string message) {
  return Status::Error(StatusCode::kUnavailable, std::move(message));
}

Status Unsupported(std::string message) {
  return Status::Error(StatusCode::kUnsupported, std::move(message));
}

Status FailedPrecondition(std::string message) {
  return Status::Error(StatusCode::kFailedPrecondition, std::move(message));
}

bool HasControlByte(const std::string& value) {
  for (const unsigned char byte : value) {
    if (byte <= 0x1f || byte == 0x7f) {
      return true;
    }
  }
  return false;
}

bool HasEmbeddedNul(const std::string& value) {
  return value.find('\0') != std::string::npos;
}

Status ParseOptionalString(const nlohmann::json& document, const char* key,
                           std::optional<std::string>* value) {
  const auto found = document.find(key);
  if (found == document.end()) {
    value->reset();
    return Status::Ok();
  }
  if (!found->is_string()) {
    return Invalid(std::string("BrowserService command field ") + key +
                   " must be a string");
  }
  const std::string& parsed = found->get_ref<const std::string&>();
  if (parsed.size() > kMaximumBrowserWindowTitleBytes ||
      HasControlByte(parsed)) {
    return Invalid(std::string("BrowserService command field ") + key +
                   " is invalid");
  }
  *value = parsed;
  return Status::Ok();
}

Status ParseOptionalBoolean(const nlohmann::json& document, const char* key,
                            std::optional<bool>* value) {
  const auto found = document.find(key);
  if (found == document.end()) {
    value->reset();
    return Status::Ok();
  }
  if (!found->is_boolean()) {
    return Invalid(std::string("BrowserService command field ") + key +
                   " must be a boolean");
  }
  *value = found->get<bool>();
  return Status::Ok();
}

Status ParseOpenCommand(const nlohmann::json& document,
                        RobloxBrowserServiceOpenRequest* request) {
  const auto url = document.find("url");
  if (url == document.end() || !url->is_string()) {
    return Invalid("BrowserService open command requires a string url");
  }
  Status status = ParseRobloxBrowserServiceOpenWindow(
      url->get_ref<const std::string&>(), request);
  if (!status.ok()) {
    return status;
  }

  std::optional<std::string> title;
  std::optional<bool> visible;
  std::optional<bool> modal;
  std::optional<bool> back_navigation_disabled;
  std::optional<bool> show_domain_as_title;
  status = ParseOptionalString(document, "title", &title);
  if (status.ok()) {
    status = ParseOptionalBoolean(document, "visible", &visible);
  }
  if (status.ok()) {
    status = ParseOptionalBoolean(document, "modal", &modal);
  }
  if (status.ok()) {
    status = ParseOptionalBoolean(document, "backNavigationDisabled",
                                  &back_navigation_disabled);
  }
  if (status.ok()) {
    status = ParseOptionalBoolean(document, "showDomainAsTitle",
                                  &show_domain_as_title);
  }
  if (!status.ok()) {
    *request = {};
    return status;
  }

  request->title = title.value_or("");
  request->visible = visible.value_or(true);
  request->modal = modal.value_or(false);
  request->back_navigation_disabled = back_navigation_disabled.value_or(false);
  request->show_domain_as_title = show_domain_as_title.value_or(false);
  return Status::Ok();
}

Status ParseConfigCommand(const nlohmann::json& document,
                          RobloxBrowserServiceConfigRequest* request) {
  *request = {};
  Status status = ParseOptionalString(document, "title", &request->title);
  if (status.ok()) {
    status = ParseOptionalBoolean(document, "visible", &request->visible);
  }
  if (status.ok()) {
    status = ParseOptionalBoolean(document, "backNavigationDisabled",
                                  &request->back_navigation_disabled);
  }
  if (!status.ok()) {
    *request = {};
  }
  return status;
}

} // namespace

struct RobloxBrowserServiceBridge::CallbackTarget {
  std::mutex mutex;
  RobloxBrowserServiceBridge *bridge = nullptr;
  EventKind kind = EventKind::kOpenWindow;
};

struct RobloxBrowserServiceBridge::BrowserWindowCloseTarget {
  std::mutex mutex;
  RobloxBrowserServiceBridge* bridge = nullptr;
  uint64_t active_generation = 0;
};

struct RobloxBrowserServiceBridge::BrowserWindowExitContext {
  std::shared_ptr<BrowserWindowCloseTarget> target;
  uint64_t generation = 0;
};

Status ParseRobloxBrowserServiceOpenWindow(
    const std::string& payload, RobloxBrowserServiceOpenRequest* request) {
  if (request == nullptr) {
    return Invalid("BrowserService open output is null");
  }
  *request = {};
  if (payload.empty() ||
      payload.size() > kMaximumRobloxBrowserServiceUrlBytes ||
      HasControlByte(payload)) {
    return Invalid("BrowserService open URL is invalid");
  }
  request->url = payload;
  return Status::Ok();
}

Status ParseRobloxBrowserServiceSendCommand(
    const std::string& json, RobloxBrowserServiceCommand* command) {
  if (command == nullptr) {
    return Invalid("BrowserService command output is null");
  }
  *command = RobloxBrowserServiceOpenRequest{};
  if (json.empty() || json.size() > kMaximumRobloxBrowserServicePayloadBytes) {
    return Invalid("BrowserService command JSON size is invalid");
  }
  const nlohmann::json document =
      nlohmann::json::parse(json.begin(), json.end(), nullptr, false);
  if (document.is_discarded() || !document.is_object()) {
    return Invalid("BrowserService command payload is not a JSON object");
  }
  const auto command_field = document.find("command");
  if (command_field == document.end() || !command_field->is_string()) {
    return Invalid("BrowserService command requires a string command");
  }
  const std::string& command_name =
      command_field->get_ref<const std::string&>();
  if (command_name == "open") {
    RobloxBrowserServiceOpenRequest request;
    const Status status = ParseOpenCommand(document, &request);
    if (status.ok()) {
      *command = std::move(request);
    }
    return status;
  }
  if (command_name == "config") {
    RobloxBrowserServiceConfigRequest request;
    const Status status = ParseConfigCommand(document, &request);
    if (status.ok()) {
      *command = std::move(request);
    }
    return status;
  }
  return Unsupported("BrowserService command is unsupported");
}

RobloxBrowserServiceBridge::RobloxBrowserServiceBridge(
    JniEnvironmentProvider environment, RobloxBrowserServiceSymbols symbols,
    RobloxBrowserServiceCallbackFactory factory, RobloxBrowserServiceSink sink)
    : environment_(environment),
      symbols_(symbols),
      factory_(factory),
      sink_(sink) {}

RobloxBrowserServiceBridge::~RobloxBrowserServiceBridge() { Shutdown(); }

Status RobloxBrowserServiceBridge::Initialize() {
  auto browser_window_close_target =
      std::make_shared<BrowserWindowCloseTarget>();
  browser_window_close_target->bridge = this;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initializing_ || shutting_down_ || initialized_) {
      return FailedPrecondition("BrowserService bridge is already initialized");
    }
    if (!environment_.valid() || !symbols_.complete() || !factory_.complete() ||
        !sink_.valid()) {
      return Unavailable("BrowserService prerequisites are incomplete");
    }
    initializing_ = true;
    accepting_messages_ = true;
    browser_window_close_target_ = browser_window_close_target;
  }

  auto finish_failed_initialization = [this](Status status) {
    std::shared_ptr<BrowserWindowCloseTarget> browser_window_close_target;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      accepting_messages_ = false;
      initializing_ = false;
      browser_window_close_target = std::move(browser_window_close_target_);
      outgoing_events_.clear();
      outgoing_event_bytes_ = 0;
      outgoing_drain_events_ = 0;
      outgoing_drain_bytes_ = 0;
      outgoing_drain_active_ = false;
      dispatch_drained_.notify_all();
    }
    if (browser_window_close_target != nullptr) {
      std::lock_guard<std::mutex> target_lock(
          browser_window_close_target->mutex);
      browser_window_close_target->bridge = nullptr;
      browser_window_close_target->active_generation = 0;
    }
    return status;
  };

  JNIEnv *env = nullptr;
  Status status = environment_.Acquire(&env);
  if (!status.ok()) {
    return finish_failed_initialization(status);
  }

  jclass mem_storage_class = env->FindClass(kMemStorageClass);
  jclass local_connection_class = env->FindClass(kMemStorageConnectionClass);
  jclass connection_class =
      local_connection_class != nullptr
          ? static_cast<jclass>(env->NewGlobalRef(local_connection_class))
          : nullptr;
  if (local_connection_class != nullptr) {
    env->DeleteLocalRef(local_connection_class);
  }
  const jfieldID connection_ref_field =
      connection_class != nullptr
          ? env->GetFieldID(connection_class, "ref", "J")
          : nullptr;
  status = CheckJniException(env, "resolve BrowserService MemStorage classes");
  if (!status.ok() || mem_storage_class == nullptr ||
      connection_class == nullptr || connection_ref_field == nullptr) {
    if (mem_storage_class != nullptr) {
      env->DeleteLocalRef(mem_storage_class);
    }
    if (connection_class != nullptr) {
      env->DeleteGlobalRef(connection_class);
    }
    return finish_failed_initialization(
        status.ok()
            ? Unavailable("BrowserService MemStorage classes are unavailable")
            : status);
  }

  std::array<jobject, 4> callbacks{};
  std::array<jobject, 4> connections{};
  std::array<std::shared_ptr<CallbackTarget>, 4> targets{};
  const std::array<const char*, 4> keys = {
      kRobloxBrowserServiceOpenWindowKey,
      kRobloxBrowserServiceCloseWindowKey,
      kRobloxBrowserServiceSendCommandKey,
      kRobloxBrowserServiceExecuteJavaScriptKey,
  };
  const std::array<EventKind, 4> kinds = {
      EventKind::kOpenWindow,
      EventKind::kCloseWindow,
      EventKind::kSendCommand,
      EventKind::kExecuteJavaScript,
  };

  auto release_resources = [&]() {
    Status cleanup_status = Status::Ok();
    {
      std::unique_lock<std::mutex> lock(mutex_);
      accepting_messages_ = false;
      dispatch_drained_.wait(lock,
                             [this] { return in_flight_dispatches_ == 0; });
    }
    for (std::size_t index = 0; index < connections.size(); ++index) {
      if (connections[index] != nullptr) {
        symbols_.disconnect(env, connection_class, connections[index]);
        Status operation_status =
            CheckJniException(env, "disconnect BrowserService MemStorage key");
        if (cleanup_status.ok()) {
          cleanup_status = operation_status;
        }
        symbols_.release_connection(env, connection_class, connections[index]);
        operation_status = CheckJniException(
            env, "release BrowserService MemStorage connection");
        if (cleanup_status.ok()) {
          cleanup_status = operation_status;
        }
      }
      if (targets[index] != nullptr) {
        std::lock_guard<std::mutex> target_lock(targets[index]->mutex);
        targets[index]->bridge = nullptr;
      }
      if (callbacks[index] != nullptr) {
        factory_.clear_callback(factory_.context, callbacks[index]);
      }
      if (connections[index] != nullptr) {
        env->DeleteGlobalRef(connections[index]);
      }
      if (callbacks[index] != nullptr) {
        env->DeleteGlobalRef(callbacks[index]);
      }
    }
    env->DeleteGlobalRef(connection_class);
    return cleanup_status;
  };

  for (std::size_t index = 0; index < keys.size(); ++index) {
    targets[index] = std::make_shared<CallbackTarget>();
    targets[index]->bridge = this;
    targets[index]->kind = kinds[index];
    jobject local_callback =
        factory_.create_callback(factory_.context, targets[index],
                                 &RobloxBrowserServiceBridge::ItemSetCallback);
    callbacks[index] =
        local_callback != nullptr ? env->NewGlobalRef(local_callback) : nullptr;
    if (local_callback != nullptr) {
      env->DeleteLocalRef(local_callback);
    }
    status = CheckJniException(env, "retain BrowserService callback");
    if (!status.ok() || callbacks[index] == nullptr) {
      if (status.ok()) {
        status = Unavailable("could not retain BrowserService callback");
      }
      break;
    }

    jstring key = env->NewStringUTF(keys[index]);
    jobject local_connection =
        key != nullptr
            ? symbols_.bind(env, mem_storage_class, key, callbacks[index])
            : nullptr;
    if (key != nullptr) {
      env->DeleteLocalRef(key);
    }
    status = CheckJniException(env, "bind BrowserService MemStorage key");
    if (status.ok() && local_connection != nullptr) {
      const jlong connection_ref =
          env->GetLongField(local_connection, connection_ref_field);
      status = CheckJniException(env, "read BrowserService connection ref");
      if (status.ok() && connection_ref != 0) {
        connections[index] = env->NewGlobalRef(local_connection);
        status = CheckJniException(env, "retain BrowserService connection");
      } else if (status.ok()) {
        status = FailedPrecondition(
            "MemStorage Connection.<init>(J) ref was not preserved");
      }
    }
    if (local_connection != nullptr) {
      if (!status.ok() || connections[index] == nullptr) {
        symbols_.disconnect(env, connection_class, local_connection);
        (void)CheckJniException(
            env, "disconnect failed BrowserService initialization");
        symbols_.release_connection(env, connection_class, local_connection);
        (void)CheckJniException(env,
                                "release failed BrowserService initialization");
      }
      env->DeleteLocalRef(local_connection);
    }
    if (!status.ok() || connections[index] == nullptr) {
      if (status.ok()) {
        status = Unavailable("BrowserService MemStorage bind failed");
      }
      break;
    }
  }
  env->DeleteLocalRef(mem_storage_class);

  if (!status.ok()) {
    const Status cleanup_status = release_resources();
    if (status.ok()) {
      status = cleanup_status;
    }
    return finish_failed_initialization(status);
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    connection_class_ = connection_class;
    callbacks_ = callbacks;
    connections_ = connections;
    callback_targets_ = std::move(targets);
    initialized_ = true;
    initializing_ = false;
    dispatch_drained_.notify_all();
  }
  return Status::Ok();
}

Status RobloxBrowserServiceBridge::Shutdown() {
  std::unique_lock<std::mutex> lock(mutex_);
  accepting_messages_ = false;
  dispatch_drained_.wait(lock,
                         [this] { return !initializing_ && !shutting_down_; });
  if (!initialized_ && connection_class_ == nullptr) {
    return Status::Ok();
  }
  shutting_down_ = true;
  const jclass connection_class = connection_class_;
  const std::array<jobject, 4> callbacks = callbacks_;
  const std::array<jobject, 4> connections = connections_;
  const std::array<std::shared_ptr<CallbackTarget>, 4> targets =
      callback_targets_;
  const std::shared_ptr<BrowserWindowCloseTarget> browser_window_close_target =
      browser_window_close_target_;
  lock.unlock();

  // Callback.onItemSet acquires the callback-target mutex before reserving a
  // dispatch under mutex_. Make every target inert before waiting for active
  // dispatches so shutdown never acquires those locks in the reverse order.
  // This also prevents retained Java callbacks from reaching a destroyed
  // bridge when acquiring a JNIEnv for cleanup fails.
  for (const std::shared_ptr<CallbackTarget> &target : targets) {
    if (target != nullptr) {
      std::lock_guard<std::mutex> target_lock(target->mutex);
      target->bridge = nullptr;
    }
  }
  if (browser_window_close_target != nullptr) {
    std::lock_guard<std::mutex> target_lock(browser_window_close_target->mutex);
    browser_window_close_target->bridge = nullptr;
    browser_window_close_target->active_generation = 0;
  }

  lock.lock();
  dispatch_drained_.wait(lock, [this] { return in_flight_dispatches_ == 0; });
  lock.unlock();

  JNIEnv *env = nullptr;
  Status status = environment_.Acquire(&env);
  if (!status.ok()) {
    lock.lock();
    shutting_down_ = false;
    dispatch_drained_.notify_all();
    return status;
  }

  for (jobject connection : connections) {
    if (connection == nullptr) {
      continue;
    }
    symbols_.disconnect(env, connection_class, connection);
    Status operation_status =
        CheckJniException(env, "disconnect BrowserService MemStorage key");
    if (status.ok()) {
      status = operation_status;
    }
    symbols_.release_connection(env, connection_class, connection);
    operation_status =
        CheckJniException(env, "release BrowserService MemStorage connection");
    if (status.ok()) {
      status = operation_status;
    }
  }

  for (std::size_t index = 0; index < callbacks.size(); ++index) {
    if (callbacks[index] != nullptr) {
      factory_.clear_callback(factory_.context, callbacks[index]);
    }
    if (connections[index] != nullptr) {
      env->DeleteGlobalRef(connections[index]);
    }
    if (callbacks[index] != nullptr) {
      env->DeleteGlobalRef(callbacks[index]);
    }
  }
  if (connection_class != nullptr) {
    env->DeleteGlobalRef(connection_class);
  }

  lock.lock();
  connection_class_ = nullptr;
  callbacks_ = {};
  connections_ = {};
  callback_targets_ = {};
  browser_window_close_target_.reset();
  outgoing_events_.clear();
  outgoing_event_bytes_ = 0;
  outgoing_drain_events_ = 0;
  outgoing_drain_bytes_ = 0;
  outgoing_drain_active_ = false;
  initialized_ = false;
  shutting_down_ = false;
  dispatch_drained_.notify_all();
  return status;
}

Status RobloxBrowserServiceBridge::HandleOwnedOpenWindow(std::string payload) {
  RobloxBrowserServiceOpenRequest request;
  Status status = ParseRobloxBrowserServiceOpenWindow(payload, &request);
  if (!status.ok()) {
    return status;
  }
  return DispatchOpenRequest(std::move(request));
}

Status RobloxBrowserServiceBridge::HandleOwnedSendCommand(std::string payload) {
  RobloxBrowserServiceCommand command;
  Status status = ParseRobloxBrowserServiceSendCommand(payload, &command);
  if (!status.ok()) {
    return status;
  }
  if (const auto* open =
          std::get_if<RobloxBrowserServiceOpenRequest>(&command)) {
    return DispatchOpenRequest(*open);
  }
  status = BeginDispatch();
  if (!status.ok()) {
    return status;
  }
  if (const auto* config =
          std::get_if<RobloxBrowserServiceConfigRequest>(&command)) {
    status = sink_.dispatch_config(sink_.context, *config);
  } else {
    status = Unsupported("BrowserService command variant is unsupported");
  }
  EndDispatch();
  return status;
}

Status RobloxBrowserServiceBridge::HandleOwnedExecuteJavaScript(
    std::string payload) {
  if (payload.size() > kMaximumRobloxBrowserServicePayloadBytes ||
      HasEmbeddedNul(payload)) {
    return Invalid("BrowserService JavaScript payload is invalid");
  }
  Status status = BeginDispatch();
  if (!status.ok()) {
    return status;
  }
  status = sink_.dispatch_execute(
      sink_.context, RobloxBrowserServiceExecuteRequest{std::move(payload)});
  EndDispatch();
  return status;
}

Status RobloxBrowserServiceBridge::HandleCloseWindow() {
  Status status = BeginDispatch();
  if (!status.ok()) {
    return status;
  }
  status = sink_.dispatch_close(sink_.context);
  EndDispatch();
  return status;
}

Status RobloxBrowserServiceBridge::QueueJavaScriptCallback(
    std::string payload) {
  if (payload.size() > kMaximumRobloxBrowserServicePayloadBytes ||
      HasEmbeddedNul(payload)) {
    return Invalid("BrowserService JavaScript callback payload is invalid");
  }
  return QueueOutgoingEvent(OutgoingEventKind::kJavaScriptCallback,
                            std::move(payload));
}

Status RobloxBrowserServiceBridge::QueueBrowserWindowClosed() {
  return QueueOutgoingEvent(OutgoingEventKind::kBrowserWindowClosed, {});
}

Status RobloxBrowserServiceBridge::DrainOutgoingEvents() {
  std::deque<OutgoingEvent> events;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_ || !accepting_messages_) {
      return FailedPrecondition(
          "BrowserService bridge is not accepting outgoing events");
    }
    if (outgoing_drain_active_) {
      return FailedPrecondition(
          "BrowserService outgoing drain is already active");
    }
    if (outgoing_events_.empty()) {
      return Status::Ok();
    }
    outgoing_drain_active_ = true;
    outgoing_drain_events_ = outgoing_events_.size();
    outgoing_drain_bytes_ = outgoing_event_bytes_;
    outgoing_event_bytes_ = 0;
    events.swap(outgoing_events_);
    ++in_flight_dispatches_;
  }

  JNIEnv* env = nullptr;
  Status status = environment_.Acquire(&env);
  if (!status.ok()) {
    FinishOutgoingDrain(std::move(events));
    return status;
  }
  jclass mem_storage_class = env->FindClass(kMemStorageClass);
  status = CheckJniException(env, "resolve BrowserService outgoing class");
  if (!status.ok() || mem_storage_class == nullptr) {
    if (mem_storage_class != nullptr) {
      env->DeleteLocalRef(mem_storage_class);
    }
    FinishOutgoingDrain(std::move(events));
    return status.ok()
               ? Unavailable("BrowserService MemStorage class is unavailable")
               : status;
  }

  std::size_t delivered = 0;
  for (; delivered < events.size(); ++delivered) {
    const OutgoingEvent& event = events[delivered];
    const char* key = event.kind == OutgoingEventKind::kJavaScriptCallback
                          ? kRobloxBrowserServiceJavaScriptCallbackKey
                          : kRobloxBrowserServiceBrowserWindowClosedKey;
    jstring jni_key = env->NewStringUTF(key);
    jstring jni_payload = env->NewStringUTF(event.payload.c_str());
    if (jni_key == nullptr || jni_payload == nullptr) {
      if (jni_payload != nullptr) {
        env->DeleteLocalRef(jni_payload);
      }
      if (jni_key != nullptr) {
        env->DeleteLocalRef(jni_key);
      }
      status = Unavailable("could not allocate BrowserService outgoing event");
      break;
    }
    symbols_.fire(env, mem_storage_class, jni_key, jni_payload);
    env->DeleteLocalRef(jni_payload);
    env->DeleteLocalRef(jni_key);
    status = CheckJniException(env, "fire BrowserService outgoing event");
    if (!status.ok()) {
      break;
    }
  }
  env->DeleteLocalRef(mem_storage_class);

  if (delivered != 0) {
    events.erase(events.begin(),
                 events.begin() + static_cast<std::ptrdiff_t>(delivered));
  }
  FinishOutgoingDrain(std::move(events));
  return status;
}

bool RobloxBrowserServiceBridge::initialized() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return initialized_;
}

Status RobloxBrowserServiceBridge::CheckJniException(
    JNIEnv* env, const char* operation) const {
  if (env == nullptr) {
    return Unavailable("JNIEnv is unavailable for BrowserService");
  }
  if (env->ExceptionCheck() == JNI_FALSE) {
    return Status::Ok();
  }
  env->ExceptionClear();
  return Status::Error(
      StatusCode::kPlatformError,
      std::string(operation != nullptr ? operation : "JNI operation") +
          " raised a JNI exception");
}

Status RobloxBrowserServiceBridge::BeginDispatch() {
  std::lock_guard<std::mutex> lock(mutex_);
  if ((!initializing_ && !initialized_) || !accepting_messages_) {
    return FailedPrecondition("BrowserService bridge is not accepting events");
  }
  ++in_flight_dispatches_;
  return Status::Ok();
}

void RobloxBrowserServiceBridge::EndDispatch() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (in_flight_dispatches_ > 0) {
    --in_flight_dispatches_;
  }
  if (in_flight_dispatches_ == 0) {
    dispatch_drained_.notify_all();
  }
}

Status RobloxBrowserServiceBridge::DispatchOpenRequest(
    RobloxBrowserServiceOpenRequest request) {
  Status status = BeginDispatch();
  if (!status.ok()) {
    return status;
  }

  std::shared_ptr<BrowserWindowCloseTarget> target;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    target = browser_window_close_target_;
  }
  const uint64_t generation =
      next_browser_window_generation_.fetch_add(1, std::memory_order_relaxed);
  auto exit_context = std::make_shared<BrowserWindowExitContext>();
  exit_context->target = target;
  exit_context->generation = generation;

  uint64_t previous_generation = 0;
  bool generation_activated = false;
  if (target == nullptr || generation == 0) {
    status = FailedPrecondition(
        "BrowserService browser-window generation is unavailable");
  } else {
    std::lock_guard<std::mutex> target_lock(target->mutex);
    if (target->bridge != this) {
      status = FailedPrecondition(
          "BrowserService browser-window observer is inactive");
    } else {
      previous_generation = target->active_generation;
      target->active_generation = generation;
      generation_activated = true;
    }
  }

  if (status.ok()) {
    status = sink_.dispatch_open(
        sink_.context, request,
        WebViewHelperExitObserver{
            exit_context, &RobloxBrowserServiceBridge::BrowserWindowExited});
  }
  if (!status.ok() && generation_activated) {
    std::lock_guard<std::mutex> target_lock(target->mutex);
    if (target->bridge == this && target->active_generation == generation) {
      target->active_generation = previous_generation;
    }
  }
  EndDispatch();
  return status;
}

Status RobloxBrowserServiceBridge::QueueOutgoingEvent(OutgoingEventKind kind,
                                                      std::string payload) {
  std::lock_guard<std::mutex> lock(mutex_);
  if ((!initializing_ && !initialized_) || !accepting_messages_) {
    return FailedPrecondition(
        "BrowserService bridge is not accepting outgoing events");
  }
  const std::size_t total_events =
      outgoing_events_.size() + outgoing_drain_events_;
  const std::size_t total_bytes = outgoing_event_bytes_ + outgoing_drain_bytes_;
  if (total_events >= kMaximumRobloxBrowserServiceQueuedEvents ||
      total_bytes > kMaximumRobloxBrowserServiceQueuedBytes ||
      payload.size() > kMaximumRobloxBrowserServiceQueuedBytes - total_bytes) {
    return Unavailable("BrowserService outgoing event queue is full");
  }
  outgoing_event_bytes_ += payload.size();
  outgoing_events_.push_back({kind, std::move(payload)});
  return Status::Ok();
}

void RobloxBrowserServiceBridge::FinishOutgoingDrain(
    std::deque<OutgoingEvent> remaining_events) {
  std::lock_guard<std::mutex> lock(mutex_);
  std::size_t remaining_bytes = 0;
  for (const OutgoingEvent& event : remaining_events) {
    remaining_bytes += event.payload.size();
  }
  if (!remaining_events.empty()) {
    outgoing_events_.insert(outgoing_events_.begin(),
                            std::make_move_iterator(remaining_events.begin()),
                            std::make_move_iterator(remaining_events.end()));
    outgoing_event_bytes_ += remaining_bytes;
  }
  outgoing_drain_events_ = 0;
  outgoing_drain_bytes_ = 0;
  outgoing_drain_active_ = false;
  if (in_flight_dispatches_ > 0) {
    --in_flight_dispatches_;
  }
  if (in_flight_dispatches_ == 0) {
    dispatch_drained_.notify_all();
  }
}

Status RobloxBrowserServiceBridge::HandleItemSet(EventKind kind, JNIEnv *env,
                                                 jstring payload) {
  if (env == nullptr || payload == nullptr) {
    return Invalid("BrowserService Callback.onItemSet payload is null");
  }
  const jsize size = env->GetStringUTFLength(payload);
  if (size < 0 || static_cast<std::size_t>(size) >
                      kMaximumRobloxBrowserServicePayloadBytes) {
    return Invalid("BrowserService Callback.onItemSet payload is too large");
  }
  const char *chars = env->GetStringUTFChars(payload, nullptr);
  if (chars == nullptr) {
    return Unavailable("could not copy BrowserService callback payload");
  }
  std::string owned(chars, static_cast<std::size_t>(size));
  env->ReleaseStringUTFChars(payload, chars);
  Status status =
      CheckJniException(env, "copy BrowserService callback payload");
  if (!status.ok()) {
    return status;
  }

  switch (kind) {
  case EventKind::kOpenWindow:
    return HandleOwnedOpenWindow(std::move(owned));
  case EventKind::kCloseWindow:
    return HandleCloseWindow();
  case EventKind::kSendCommand:
    return HandleOwnedSendCommand(std::move(owned));
  case EventKind::kExecuteJavaScript:
    return HandleOwnedExecuteJavaScript(std::move(owned));
  }
  return Unsupported("BrowserService event kind is unsupported");
}

void RobloxBrowserServiceBridge::ItemSetCallback(void *context, JNIEnv *env,
                                                 jstring payload) {
  auto *target = static_cast<CallbackTarget *>(context);
  if (target == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(target->mutex);
  if (target->bridge == nullptr) {
    return;
  }
  const char *event_name = "unknown";
  switch (target->kind) {
  case EventKind::kOpenWindow:
    event_name = "OpenBrowserWindow";
    break;
  case EventKind::kCloseWindow:
    event_name = "CloseBrowserWindow";
    break;
  case EventKind::kSendCommand:
    event_name = "SendCommand";
    break;
  case EventKind::kExecuteJavaScript:
    event_name = "ExecuteJavaScript";
    break;
  }
  std::fprintf(stderr, "  [browser] BrowserService event received: %s\n",
               event_name);
  const Status status =
      target->bridge->HandleItemSet(target->kind, env, payload);
  if (!status.ok()) {
    std::fprintf(stderr, "  [browser] BrowserService event rejected: %s\n",
                 status.message().c_str());
  }
}

void RobloxBrowserServiceBridge::BrowserWindowExited(void* context) {
  auto* exit_context = static_cast<BrowserWindowExitContext*>(context);
  if (exit_context == nullptr || exit_context->target == nullptr ||
      exit_context->generation == 0) {
    return;
  }

  std::lock_guard<std::mutex> target_lock(exit_context->target->mutex);
  RobloxBrowserServiceBridge* bridge = exit_context->target->bridge;
  if (bridge == nullptr ||
      exit_context->target->active_generation != exit_context->generation) {
    return;
  }
  exit_context->target->active_generation = 0;
  const Status status = bridge->QueueBrowserWindowClosed();
  if (!status.ok()) {
    std::fprintf(stderr,
                 "  [browser] BrowserService window exit could not be queued: "
                 "%s\n",
                 status.message().c_str());
  }
}

} // namespace runtime
} // namespace mocktail
