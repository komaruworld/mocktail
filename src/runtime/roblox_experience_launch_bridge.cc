#include "runtime/roblox_experience_launch_bridge.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <limits>
#include <string>
#include <utility>

namespace mocktail {
namespace runtime {
namespace {

constexpr char kExperienceProtocolClass[] =
    "com/roblox/universalapp/experience/JNIExperienceProtocol";
constexpr char kConnectionPointerField[] = "f10205a";
constexpr std::size_t kMaximumLaunchStringBytes = 16 * 1024;

Status Invalid(std::string message) {
  return Status::Error(StatusCode::kInvalidArgument, std::move(message));
}

Status Unavailable(std::string message) {
  return Status::Error(StatusCode::kUnavailable, std::move(message));
}

Status FailedPrecondition(std::string message) {
  return Status::Error(StatusCode::kFailedPrecondition, std::move(message));
}

Status ReadOptionalInteger(const nlohmann::json &object, const char *name,
                           int64_t *value) {
  const auto found = object.find(name);
  if (found == object.end() || found->is_null()) {
    return Status::Ok();
  }
  if (!found->is_number_integer()) {
    return Invalid(std::string("experience launch field is not an integer: ") +
                   name);
  }
  if (found->is_number_unsigned() &&
      found->get<uint64_t>() >
          static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
    return Invalid(std::string("experience launch integer is out of range: ") +
                   name);
  }
  *value = found->get<int64_t>();
  return Status::Ok();
}

Status ReadOptionalString(const nlohmann::json &object, const char *name,
                          std::string *value) {
  const auto found = object.find(name);
  if (found == object.end() || found->is_null()) {
    return Status::Ok();
  }
  if (!found->is_string()) {
    return Invalid(std::string("experience launch field is not a string: ") +
                   name);
  }
  if (found->get_ref<const std::string &>().size() >
      kMaximumLaunchStringBytes) {
    return Invalid(std::string("experience launch field is too large: ") +
                   name);
  }
  *value = found->get<std::string>();
  return Status::Ok();
}

} // namespace

struct RobloxExperienceLaunchBridge::RawCallbackTarget {
  std::mutex mutex;
  RobloxExperienceLaunchBridge *bridge = nullptr;
};

Status ParseRobloxExperienceLaunchJson(const std::string &json,
                                       RobloxExperienceLaunchRequest *request) {
  if (request == nullptr) {
    return Invalid("experience launch output is null");
  }
  *request = {};
  if (json.empty() || json.size() > kMaximumExperienceLaunchJsonBytes) {
    return Invalid("experience launch JSON size is invalid");
  }

  const nlohmann::json document =
      nlohmann::json::parse(json.begin(), json.end(), nullptr, false);
  if (document.is_discarded() || !document.is_object()) {
    return Invalid("experience launch payload is not a JSON object");
  }

  RobloxExperienceLaunchRequest parsed;
  Status status = ReadOptionalInteger(document, "placeId", &parsed.place_id);
  if (status.ok()) {
    status = ReadOptionalInteger(document, "userId", &parsed.user_id);
  }
  if (status.ok()) {
    status = ReadOptionalInteger(document, "conversationId",
                                 &parsed.conversation_id);
  }
  if (status.ok()) {
    status = ReadOptionalInteger(document, "referredByPlayerId",
                                 &parsed.referred_by_player_id);
  }
  struct StringField {
    const char *name;
    std::string RobloxExperienceLaunchRequest::*member;
  };
  constexpr StringField kStringFields[] = {
      {"gameInstanceId", &RobloxExperienceLaunchRequest::game_instance_id},
      {"reservedServerAccessCode",
       &RobloxExperienceLaunchRequest::reserved_server_access_code},
      {"callId", &RobloxExperienceLaunchRequest::call_id},
      {"referralPage", &RobloxExperienceLaunchRequest::referral_page},
      {"accessCode", &RobloxExperienceLaunchRequest::access_code},
      {"linkCode", &RobloxExperienceLaunchRequest::link_code},
      {"launchData", &RobloxExperienceLaunchRequest::launch_data},
      {"eventId", &RobloxExperienceLaunchRequest::event_id},
      {"gameJoinContext", &RobloxExperienceLaunchRequest::game_join_context},
      {"joinAttemptId", &RobloxExperienceLaunchRequest::join_attempt_id},
      {"joinAttemptOrigin",
       &RobloxExperienceLaunchRequest::join_attempt_origin},
      {"isoContext", &RobloxExperienceLaunchRequest::iso_context},
  };
  for (const StringField &field : kStringFields) {
    if (!status.ok()) {
      break;
    }
    status = ReadOptionalString(document, field.name, &(parsed.*field.member));
  }
  if (!status.ok()) {
    return status;
  }
  // This matches APK ug.l: a launch must identify a place or a user. Mocktail
  // currently supports place joins only, so reject follow-user requests at the
  // boundary rather than silently launching place 0.
  if (parsed.place_id <= 0) {
    return Invalid("experience launch requires a positive placeId");
  }
  if (parsed.user_id < 0 || parsed.conversation_id < 0 ||
      parsed.referred_by_player_id < 0) {
    return Invalid("experience launch identifiers cannot be negative");
  }
  parsed.canonical_json = document.dump();
  if (parsed.canonical_json.size() > kMaximumExperienceLaunchJsonBytes) {
    return Invalid("canonical experience launch JSON is too large");
  }
  *request = std::move(parsed);
  return Status::Ok();
}

GameJoinRequest
MakeGameJoinRequest(uint64_t request_id,
                    const RobloxExperienceLaunchRequest &request) {
  return {request_id, request.place_id, request.canonical_json};
}

RobloxExperienceLaunchBridge::RobloxExperienceLaunchBridge(
    JniEnvironmentProvider environment,
    RobloxExperienceMessageBusSymbols symbols,
    RobloxExperienceMessageBusObjects objects, RobloxExperienceLaunchSink sink)
    : environment_(environment), symbols_(symbols), source_objects_(objects),
      sink_(sink) {}

RobloxExperienceLaunchBridge::~RobloxExperienceLaunchBridge() { Shutdown(); }

Status RobloxExperienceLaunchBridge::Initialize() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initializing_ || shutting_down_ || initialized_) {
      return FailedPrecondition(
          "experience launch bridge is already initialized");
    }
    if (!environment_.valid() || !symbols_.complete() ||
        !source_objects_.complete() || !sink_.valid()) {
      return Unavailable(
          "experience launch MessageBus prerequisites are incomplete");
    }
    // doSubscribeRaw is allowed to invoke RawCallback synchronously. Publish
    // only the dispatch state here; JNI resources remain local until the
    // subscription has returned and its ownership contract is validated.
    initializing_ = true;
    accepting_messages_ = true;
  }

  auto finish_failed_initialization = [this](Status status) {
    std::lock_guard<std::mutex> lock(mutex_);
    accepting_messages_ = false;
    initializing_ = false;
    dispatch_drained_.notify_all();
    return status;
  };

  JNIEnv *env = nullptr;
  Status status = environment_.Acquire(&env);
  if (!status.ok()) {
    return finish_failed_initialization(status);
  }
  jclass protocol_class = env->FindClass(kExperienceProtocolClass);
  if (protocol_class == nullptr) {
    return finish_failed_initialization(
        Unavailable("JNIExperienceProtocol class is unavailable"));
  }
  jstring launch_id = symbols_.get_launch_id(env, protocol_class);
  env->DeleteLocalRef(protocol_class);
  status = CheckJniException(env, "resolve ExperienceProtocol launch id");
  if (!status.ok() || launch_id == nullptr) {
    return finish_failed_initialization(
        status.ok() ? Unavailable("ExperienceProtocol launch id is null")
                    : status);
  }

  auto raw_callback_target = std::make_shared<RawCallbackTarget>();
  raw_callback_target->bridge = this;
  jobject local_callback = source_objects_.create_raw_callback(
      source_objects_.callback_factory_context, raw_callback_target,
      &RobloxExperienceLaunchBridge::RawMessageCallback);
  jobject message_bus = env->NewGlobalRef(source_objects_.message_bus);
  jobject raw_callback =
      local_callback != nullptr ? env->NewGlobalRef(local_callback) : nullptr;
  if (message_bus == nullptr || raw_callback == nullptr) {
    env->DeleteLocalRef(launch_id);
    if (raw_callback != nullptr)
      env->DeleteGlobalRef(raw_callback);
    if (message_bus != nullptr)
      env->DeleteGlobalRef(message_bus);
    if (local_callback != nullptr) {
      source_objects_.clear_raw_callback(
          source_objects_.callback_factory_context, local_callback);
    }
    return finish_failed_initialization(
        Unavailable("could not retain ExperienceProtocol MessageBus objects"));
  }
  jobject local_connection = symbols_.subscribe_raw(env, message_bus, launch_id,
                                                    raw_callback, JNI_FALSE);
  env->DeleteLocalRef(launch_id);
  status = CheckJniException(env, "subscribe to ExperienceProtocol launch");
  if (!status.ok() || local_connection == nullptr) {
    {
      std::lock_guard<std::mutex> target_lock(raw_callback_target->mutex);
      raw_callback_target->bridge = nullptr;
    }
    source_objects_.clear_raw_callback(source_objects_.callback_factory_context,
                                       local_callback);
    env->DeleteGlobalRef(raw_callback);
    env->DeleteGlobalRef(message_bus);
    return finish_failed_initialization(
        status.ok() ? Unavailable("ExperienceProtocol subscription failed")
                    : status);
  }

  jclass connection_class = env->GetObjectClass(local_connection);
  jfieldID pointer_field =
      connection_class != nullptr
          ? env->GetFieldID(connection_class, kConnectionPointerField, "J")
          : nullptr;
  const jlong connection_handle =
      pointer_field != nullptr
          ? env->GetLongField(local_connection, pointer_field)
          : 0;
  if (connection_class != nullptr)
    env->DeleteLocalRef(connection_class);
  jobject connection = env->NewGlobalRef(local_connection);
  env->DeleteLocalRef(local_connection);
  status = CheckJniException(env, "retain ExperienceProtocol connection");
  if (!status.ok() || connection == nullptr || connection_handle == 0) {
    // If the pseudo-JVM lost Connection.<init>(J), there is no safe value to
    // pass to deleteSharedPtr. Fail closed and make the missing ownership seam
    // observable. The pseudo-JVM must preserve the constructor argument before
    // production composition enables this bridge.
    if (connection != nullptr)
      env->DeleteGlobalRef(connection);
    {
      std::lock_guard<std::mutex> target_lock(raw_callback_target->mutex);
      raw_callback_target->bridge = nullptr;
    }
    source_objects_.clear_raw_callback(source_objects_.callback_factory_context,
                                       local_callback);
    env->DeleteGlobalRef(raw_callback);
    env->DeleteGlobalRef(message_bus);
    return finish_failed_initialization(
        status.ok()
            ? FailedPrecondition(
                  "MessageBus Connection.<init>(J) handle was not preserved")
            : status);
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    message_bus_ = message_bus;
    raw_callback_ = raw_callback;
    connection_ = connection;
    connection_handle_ = connection_handle;
    raw_callback_target_ = std::move(raw_callback_target);
    initialized_ = true;
    initializing_ = false;
    dispatch_drained_.notify_all();
  }
  return Status::Ok();
}

Status RobloxExperienceLaunchBridge::Shutdown() {
  std::unique_lock<std::mutex> lock(mutex_);
  accepting_messages_ = false;
  dispatch_drained_.wait(lock,
                         [this] { return !initializing_ && !shutting_down_; });
  if (!initialized_ && connection_ == nullptr && raw_callback_ == nullptr &&
      message_bus_ == nullptr) {
    return Status::Ok();
  }
  shutting_down_ = true;
  JNIEnv *env = nullptr;
  Status status = environment_.Acquire(&env);
  if (!status.ok()) {
    shutting_down_ = false;
    dispatch_drained_.notify_all();
    return status;
  }
  jobject connection = connection_;
  const jlong handle = connection_handle_;
  const std::shared_ptr<RawCallbackTarget> raw_callback_target =
      raw_callback_target_;
  connection_handle_ = 0;
  initialized_ = false;
  lock.unlock();
  if (connection != nullptr && handle != 0) {
    symbols_.delete_connection(env, connection, handle);
    status = CheckJniException(env, "disconnect ExperienceProtocol launch");
  }
  // RawMessageCallback acquires the target mutex before BeginDispatch takes
  // mutex_. Invalidate the target without holding mutex_ so shutdown cannot
  // form the reverse mutex_ -> target lock order with a callback already in
  // HandleRawMessage.
  if (raw_callback_target != nullptr) {
    std::lock_guard<std::mutex> target_lock(raw_callback_target->mutex);
    raw_callback_target->bridge = nullptr;
  }
  lock.lock();
  dispatch_drained_.wait(lock, [this] { return in_flight_dispatches_ == 0; });
  if (raw_callback_ != nullptr) {
    source_objects_.clear_raw_callback(source_objects_.callback_factory_context,
                                       raw_callback_);
  }
  if (connection_ != nullptr)
    env->DeleteGlobalRef(connection_);
  if (raw_callback_ != nullptr)
    env->DeleteGlobalRef(raw_callback_);
  if (message_bus_ != nullptr)
    env->DeleteGlobalRef(message_bus_);
  connection_ = nullptr;
  raw_callback_ = nullptr;
  message_bus_ = nullptr;
  raw_callback_target_.reset();
  shutting_down_ = false;
  dispatch_drained_.notify_all();
  return status;
}

Status RobloxExperienceLaunchBridge::HandleRawMessage(JNIEnv *env,
                                                      jstring message) {
  if (env == nullptr || message == nullptr) {
    return Invalid("ExperienceProtocol RawCallback message is null");
  }
  const jsize size = env->GetStringUTFLength(message);
  if (size <= 0 ||
      static_cast<std::size_t>(size) > kMaximumExperienceLaunchJsonBytes) {
    return Invalid("ExperienceProtocol RawCallback message size is invalid");
  }
  const char *chars = env->GetStringUTFChars(message, nullptr);
  if (chars == nullptr) {
    return Unavailable("could not copy ExperienceProtocol launch JSON");
  }
  std::string owned(chars, static_cast<std::size_t>(size));
  env->ReleaseStringUTFChars(message, chars);
  Status status = CheckJniException(env, "copy ExperienceProtocol launch JSON");
  return status.ok() ? HandleOwnedMessage(std::move(owned)) : status;
}

Status RobloxExperienceLaunchBridge::HandleOwnedMessage(std::string message) {
  Status status = BeginDispatch();
  if (!status.ok()) {
    return status;
  }
  RobloxExperienceLaunchRequest request;
  status = ParseRobloxExperienceLaunchJson(message, &request);
  if (status.ok()) {
    status = sink_.dispatch(sink_.context, request);
  }
  EndDispatch();
  return status;
}

bool RobloxExperienceLaunchBridge::initialized() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return initialized_;
}

Status
RobloxExperienceLaunchBridge::CheckJniException(JNIEnv *env,
                                                const char *operation) const {
  if (env == nullptr) {
    return Unavailable("JNIEnv is unavailable");
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

Status RobloxExperienceLaunchBridge::BeginDispatch() {
  std::lock_guard<std::mutex> lock(mutex_);
  if ((!initializing_ && !initialized_) || !accepting_messages_) {
    return FailedPrecondition(
        "experience launch bridge is not accepting messages");
  }
  ++in_flight_dispatches_;
  return Status::Ok();
}

void RobloxExperienceLaunchBridge::EndDispatch() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (in_flight_dispatches_ > 0) {
    --in_flight_dispatches_;
  }
  if (in_flight_dispatches_ == 0) {
    dispatch_drained_.notify_all();
  }
}

void RobloxExperienceLaunchBridge::RawMessageCallback(void *context,
                                                      JNIEnv *env,
                                                      jstring message) {
  auto *target = static_cast<RawCallbackTarget *>(context);
  if (target == nullptr) {
    return;
  }
  std::lock_guard<std::mutex> lock(target->mutex);
  if (target->bridge != nullptr) {
    const Status status = target->bridge->HandleRawMessage(env, message);
    if (!status.ok()) {
      std::fprintf(stderr, "[experience] launch request rejected: %s\n",
                   status.message().c_str());
    }
  }
}

} // namespace runtime
} // namespace mocktail
