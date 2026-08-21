#include "runtime/roblox_fresh_game_launch_controller.h"

#include <utf8proc.h>

#include <cstddef>
#include <limits>
#include <utility>
#include <vector>

namespace mocktail {
namespace runtime {
namespace {

// The APK builder returns this concrete AutoValue implementation. Native code
// consumes the abstract StartGameParams accessors, which the pseudo-JVM maps to
// these concrete backing fields.
constexpr char kStartGameParamsClass[] =
    "com/roblox/engine/jni/autovalue/AutoValue_StartGameParams";
constexpr std::size_t kMaximumLaunchFieldBytes = 16 * 1024;

Status Invalid(std::string message) {
  return Status::Error(StatusCode::kInvalidArgument, std::move(message));
}

Status Unavailable(std::string message) {
  return Status::Error(StatusCode::kUnavailable, std::move(message));
}

Status FailedPrecondition(std::string message) {
  return Status::Error(StatusCode::kFailedPrecondition, std::move(message));
}

Status PlatformError(std::string message) {
  return Status::Error(StatusCode::kPlatformError, std::move(message));
}

Status CheckJni(JNIEnv* env, const char* operation) {
  if (env == nullptr) {
    return Unavailable("JNIEnv is unavailable while building StartGameParams");
  }
  if (env->ExceptionCheck() == JNI_FALSE) {
    return Status::Ok();
  }
  env->ExceptionClear();
  std::string message = operation != nullptr ? operation : "JNI operation";
  message += " failed while building fresh StartGameParams";
  return PlatformError(std::move(message));
}

jstring NewJavaString(JNIEnv* env, const std::string& utf8) {
  if (env == nullptr || utf8.size() > kMaximumLaunchFieldBytes) {
    return nullptr;
  }
  std::vector<jchar> utf16;
  utf16.reserve(utf8.size());
  std::size_t offset = 0;
  while (offset < utf8.size()) {
    utf8proc_int32_t codepoint = 0;
    const utf8proc_ssize_t count = utf8proc_iterate(
        reinterpret_cast<const utf8proc_uint8_t*>(utf8.data() + offset),
        static_cast<utf8proc_ssize_t>(utf8.size() - offset), &codepoint);
    if (count <= 0 || codepoint < 0 || codepoint > 0x10ffff) {
      return nullptr;
    }
    if (codepoint <= 0xffff) {
      utf16.push_back(static_cast<jchar>(codepoint));
    } else {
      codepoint -= 0x10000;
      utf16.push_back(static_cast<jchar>(0xd800 + (codepoint >> 10)));
      utf16.push_back(static_cast<jchar>(0xdc00 + (codepoint & 0x3ff)));
    }
    offset += static_cast<std::size_t>(count);
  }
  if (utf16.size() >
      static_cast<std::size_t>(std::numeric_limits<jsize>::max())) {
    return nullptr;
  }
  return env->NewString(utf16.data(), static_cast<jsize>(utf16.size()));
}

Status SetObject(JNIEnv* env, jobject object, jclass clazz, const char* name,
                 const char* signature, jobject value) {
  jfieldID field = env->GetFieldID(clazz, name, signature);
  if (field == nullptr) {
    Status status = CheckJni(env, name);
    return status.ok()
               ? Unavailable(
                     std::string("StartGameParams field is unavailable: ") +
                     name)
               : status;
  }
  env->SetObjectField(object, field, value);
  return CheckJni(env, name);
}

Status SetLong(JNIEnv* env, jobject object, jclass clazz, const char* name,
               int64_t value) {
  jfieldID field = env->GetFieldID(clazz, name, "J");
  if (field == nullptr) {
    Status status = CheckJni(env, name);
    return status.ok()
               ? Unavailable(
                     std::string("StartGameParams field is unavailable: ") +
                     name)
               : status;
  }
  env->SetLongField(object, field, static_cast<jlong>(value));
  return CheckJni(env, name);
}

Status SetInt(JNIEnv* env, jobject object, jclass clazz, const char* name,
              int32_t value) {
  jfieldID field = env->GetFieldID(clazz, name, "I");
  if (field == nullptr) {
    Status status = CheckJni(env, name);
    return status.ok()
               ? Unavailable(
                     std::string("StartGameParams field is unavailable: ") +
                     name)
               : status;
  }
  env->SetIntField(object, field, static_cast<jint>(value));
  return CheckJni(env, name);
}

Status SetBoolean(JNIEnv* env, jobject object, jclass clazz, const char* name,
                  bool value) {
  jfieldID field = env->GetFieldID(clazz, name, "Z");
  if (field == nullptr) {
    Status status = CheckJni(env, name);
    return status.ok()
               ? Unavailable(
                     std::string("StartGameParams field is unavailable: ") +
                     name)
               : status;
  }
  env->SetBooleanField(object, field, value ? JNI_TRUE : JNI_FALSE);
  return CheckJni(env, name);
}

Status SetString(JNIEnv* env, jobject object, jclass clazz, const char* name,
                 const std::string& value) {
  jstring local = NewJavaString(env, value);
  if (local == nullptr) {
    Status status = CheckJni(env, name);
    return status.ok()
               ? Invalid(
                     std::string(
                         "StartGameParams string is invalid or too large: ") +
                     name)
               : status;
  }
  Status status =
      SetObject(env, object, clazz, name, "Ljava/lang/String;", local);
  env->DeleteLocalRef(local);
  return status;
}

Status ValidateRequest(const RobloxExperienceLaunchRequest& request) {
  if (request.place_id <= 0 || request.user_id < 0 ||
      request.conversation_id < 0 || request.referred_by_player_id < 0) {
    return Invalid("fresh game launch identifiers are invalid");
  }
  if (request.canonical_json.empty() ||
      request.canonical_json.size() > kMaximumExperienceLaunchJsonBytes) {
    return Invalid("fresh game launch canonical JSON is unavailable");
  }
  const std::string* fields[] = {
      &request.game_instance_id,
      &request.reserved_server_access_code,
      &request.call_id,
      &request.referral_page,
      &request.access_code,
      &request.link_code,
      &request.launch_data,
      &request.event_id,
      &request.game_join_context,
      &request.join_attempt_id,
      &request.join_attempt_origin,
      &request.iso_context,
  };
  for (const std::string* field : fields) {
    if (field->size() > kMaximumLaunchFieldBytes) {
      return Invalid("fresh game launch string field is too large");
    }
  }
  RobloxExperienceLaunchRequest canonical;
  Status status =
      ParseRobloxExperienceLaunchJson(request.canonical_json, &canonical);
  if (!status.ok()) {
    return Invalid("fresh game launch canonical JSON is invalid");
  }
  if (canonical.place_id != request.place_id ||
      canonical.user_id != request.user_id ||
      canonical.conversation_id != request.conversation_id ||
      canonical.referred_by_player_id != request.referred_by_player_id ||
      canonical.game_instance_id != request.game_instance_id ||
      canonical.reserved_server_access_code !=
          request.reserved_server_access_code ||
      canonical.call_id != request.call_id ||
      canonical.referral_page != request.referral_page ||
      canonical.access_code != request.access_code ||
      canonical.link_code != request.link_code ||
      canonical.launch_data != request.launch_data ||
      canonical.event_id != request.event_id ||
      canonical.game_join_context != request.game_join_context ||
      canonical.join_attempt_id != request.join_attempt_id ||
      canonical.join_attempt_origin != request.join_attempt_origin ||
      canonical.iso_context != request.iso_context) {
    return Invalid(
        "fresh game launch fields do not match their canonical JSON");
  }
  return Status::Ok();
}

Status UpdateStatus(const GameSessionUpdateResult& result) {
  if (result.ok()) {
    return Status::Ok();
  }
  return result.cause.ok() ? FailedPrecondition(result.message) : result.cause;
}

GameSessionUpdateResult RejectSurfaceEvent(const char* message) {
  return {GameSessionUpdateStatus::kRejected, GameSessionState::kCreated,
          FailedPrecondition(message), message};
}

}  // namespace

bool RobloxFreshGameLaunchContext::complete() const {
  const bool principal_valid =
      principal.kind == GameSessionPrincipalKind::kAuthenticated &&
      principal.generation != 0 && !principal.principal_id.empty() &&
      !principal.base_url.empty();
  return request_id != 0 && native_gl_class != nullptr && surface != nullptr &&
         platform_params != nullptr && principal_valid &&
         game_surface.generation != 0 && game_surface.native_window != 0 &&
         game_surface.width != 0 && game_surface.height != 0;
}

Status BuildFreshStartGameParams(JNIEnv* env,
                                 const RobloxExperienceLaunchRequest& request,
                                 const RobloxFreshGameLaunchContext& context,
                                 jobject* result) {
  if (result == nullptr) {
    return Invalid("fresh StartGameParams output is null");
  }
  *result = nullptr;
  if (env == nullptr || !context.complete()) {
    return Invalid("fresh StartGameParams context is incomplete");
  }
  Status status = ValidateRequest(request);
  if (!status.ok()) {
    return status;
  }
  if (context.username.size() > kMaximumLaunchFieldBytes) {
    return Invalid("fresh StartGameParams username is too large");
  }

  jclass clazz = env->FindClass(kStartGameParamsClass);
  if (clazz == nullptr) {
    status = CheckJni(env, "find StartGameParams class");
    return status.ok() ? Unavailable("StartGameParams class is unavailable")
                       : status;
  }
  jobject params = env->AllocObject(clazz);
  if (params == nullptr) {
    env->DeleteLocalRef(clazz);
    status = CheckJni(env, "allocate StartGameParams");
    return status.ok() ? Unavailable("could not allocate StartGameParams")
                       : status;
  }

  status = SetObject(env, params, clazz, "surface", "Landroid/view/Surface;",
                     context.surface);
  if (status.ok()) {
    status = SetObject(env, params, clazz, "platformParams",
                       "Lcom/roblox/engine/jni/model/PlatformParams;",
                       context.platform_params);
  }
  if (status.ok()) {
    status = SetObject(env, params, clazz, "deviceParams",
                       "Lcom/roblox/engine/jni/model/DeviceParams;",
                       context.device_params);
  }
  if (status.ok())
    status = SetLong(env, params, clazz, "placeId", request.place_id);
  if (status.ok())
    status = SetLong(env, params, clazz, "userId", request.user_id);
  if (status.ok())
    status = SetString(env, params, clazz, "accessCode", request.access_code);
  if (status.ok())
    status = SetString(env, params, clazz, "callId", request.call_id);
  if (status.ok())
    status = SetString(env, params, clazz, "linkCode", request.link_code);
  if (status.ok()) {
    status = SetString(env, params, clazz, "reservedServerAccessCode",
                       request.reserved_server_access_code);
  }
  if (status.ok())
    status =
        SetLong(env, params, clazz, "conversationId", request.conversation_id);
  if (status.ok())
    status = SetInt(env, params, clazz, "joinRequestType",
                    context.join_request_type);
  if (status.ok())
    status = SetString(env, params, clazz, "gameId", request.game_instance_id);
  if (status.ok())
    status = SetBoolean(env, params, clazz, "isUnder13", context.is_under_13);
  if (status.ok())
    status = SetString(env, params, clazz, "username", context.username);
  if (status.ok())
    status =
        SetString(env, params, clazz, "referralPage", request.referral_page);
  if (status.ok())
    status = SetString(env, params, clazz, "launchData", request.launch_data);
  if (status.ok())
    status = SetString(env, params, clazz, "gameJoinContext",
                       request.game_join_context);
  if (status.ok())
    status = SetString(env, params, clazz, "eventId", request.event_id);
  if (status.ok())
    status =
        SetString(env, params, clazz, "joinAttemptId", request.join_attempt_id);
  if (status.ok())
    status = SetString(env, params, clazz, "joinAttemptOrigin",
                       request.join_attempt_origin);
  if (status.ok())
    status = SetString(env, params, clazz, "isoContext", request.iso_context);
  if (status.ok()) {
    status = SetLong(env, params, clazz, "referredByPlayerId",
                     request.referred_by_player_id);
  }
  if (status.ok()) {
    status = SetObject(env, params, clazz, "vrContext",
                       "Landroid/app/Activity;", context.vr_context);
  }
  env->DeleteLocalRef(clazz);
  if (!status.ok()) {
    env->DeleteLocalRef(params);
    return status;
  }
  *result = params;
  return Status::Ok();
}

RobloxFreshGameLaunchController::RobloxFreshGameLaunchController(
    JniEnvironmentProvider environment, RobloxGameSessionSymbols symbols,
    RobloxFreshLaunchPresentBoundary present_boundary,
    RobloxGameSurfaceJniConfig surface_config,
    RobloxGamePresentedObserver presented_observer)
    : environment_(environment),
      symbols_(symbols),
      present_boundary_(present_boundary),
      surface_config_(std::move(surface_config)),
      presented_observer_(presented_observer) {}

RobloxFreshGameLaunchController::~RobloxFreshGameLaunchController() {
  Shutdown();
}

Status RobloxFreshGameLaunchController::Launch(
    const RobloxExperienceLaunchRequest& request,
    const RobloxFreshGameLaunchContext& context) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (app_shutdown_) {
    return FailedPrecondition(
        "fresh game launch is unavailable after app runtime shutdown");
  }
  if (active_ || runtime_ != nullptr || observer_registered_) {
    return FailedPrecondition("a fresh game launch is already active");
  }
  if (!environment_.valid() || !symbols_.complete() ||
      !present_boundary_.complete() || !surface_config_.complete() ||
      !context.complete()) {
    return FailedPrecondition("fresh game launch prerequisites are incomplete");
  }
  Status status = ValidateRequest(request);
  if (!status.ok()) {
    return status;
  }
  JNIEnv* env = nullptr;
  status = environment_.Acquire(&env);
  if (!status.ok()) {
    return status;
  }
  jobject start_game_params = nullptr;
  status = BuildFreshStartGameParams(env, request, context, &start_game_params);
  if (!status.ok()) {
    return status;
  }

  runtime_ = std::make_unique<RobloxGameSessionRuntime>(
      environment_, symbols_, surface_config_, presented_observer_);
  observer_registered_ = present_boundary_.register_observer(
      present_boundary_.context,
      &RobloxGameSessionRuntime::SuccessfulPresentCallback, runtime_.get());
  if (!observer_registered_) {
    env->DeleteLocalRef(start_game_params);
    runtime_.reset();
    return Unavailable("could not register fresh-game host present observer");
  }

  const GameJoinRequest join = MakeGameJoinRequest(context.request_id, request);
  RobloxGameSessionBinding binding;
  binding.objects = {context.native_gl_class, context.surface,
                     context.platform_params, context.activity,
                     start_game_params};
  binding.principal = context.principal;
  binding.request = join;
  binding.surface = context.game_surface;
  status = runtime_->InitializeAndStart(binding, context.principal, join,
                                        context.game_surface);
  env->DeleteLocalRef(start_game_params);
  if (!status.ok()) {
    present_boundary_.clear_observer(present_boundary_.context);
    observer_registered_ = false;
    (void)runtime_->Shutdown();
    last_snapshot_ = runtime_->Snapshot();
    // Shutdown may have produced irreversible native side effects even when
    // its status is rejected. Never reuse this app runtime after an attempted
    // fail-closed cleanup.
    app_shutdown_ = true;
    runtime_.reset();
    return status;
  }
  active_ = true;
  last_snapshot_ = {};
  return Status::Ok();
}

Status RobloxFreshGameLaunchController::Leave() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!active_ || runtime_ == nullptr) {
    return FailedPrecondition("no fresh game session is active");
  }
  const GameSessionUpdateResult result = runtime_->LeaveGame();
  if (!result.ok()) {
    return UpdateStatus(result);
  }
  if (observer_registered_) {
    present_boundary_.clear_observer(present_boundary_.context);
    observer_registered_ = false;
  }
  last_snapshot_ = runtime_->Snapshot();
  runtime_.reset();
  active_ = false;
  return Status::Ok();
}

Status RobloxFreshGameLaunchController::ObserveLuaAppReturn() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!active_ && runtime_ == nullptr && !observer_registered_) {
    return Status::Ok();
  }
  if (observer_registered_) {
    // Native has already completed the transition. Drain present callbacks
    // before releasing the runtime they target, but do not replay any Roblox
    // leave, stop, or destroy entrypoint.
    present_boundary_.clear_observer(present_boundary_.context);
    observer_registered_ = false;
  }
  if (runtime_ != nullptr) {
    last_snapshot_ = runtime_->Snapshot();
    last_snapshot_.state = GameSessionState::kReturnedToLuaApp;
    last_snapshot_.returned_to_lua_app = true;
    last_snapshot_.game_running = false;
    last_snapshot_.game_paused = false;
    last_snapshot_.game_present_pending = false;
    last_snapshot_.active_surface_available = false;
    last_snapshot_.surface_created = false;
    runtime_.reset();
  }
  active_ = false;
  return Status::Ok();
}

Status RobloxFreshGameLaunchController::Shutdown() {
  std::lock_guard<std::mutex> lock(mutex_);
  app_shutdown_ = true;
  if (observer_registered_) {
    // The boundary contract drains in-flight callbacks before returning, so
    // runtime_ remains alive for every accepted present.
    present_boundary_.clear_observer(present_boundary_.context);
    observer_registered_ = false;
  }
  Status status = Status::Ok();
  if (runtime_ != nullptr) {
    const GameSessionUpdateResult shutdown = runtime_->Shutdown();
    status = UpdateStatus(shutdown);
    last_snapshot_ = runtime_->Snapshot();
    runtime_.reset();
  }
  active_ = false;
  return status;
}

GameSessionUpdateResult RobloxFreshGameLaunchController::SurfaceCreated(
    uint64_t surface_generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_ && runtime_ != nullptr
             ? runtime_->SurfaceCreated(surface_generation)
             : RejectSurfaceEvent(
                   "cannot create a surface without an active fresh game");
}

GameSessionUpdateResult RobloxFreshGameLaunchController::SurfaceChanged(
    GameSurface surface) {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_ && runtime_ != nullptr
             ? runtime_->SurfaceChanged(std::move(surface))
             : RejectSurfaceEvent(
                   "cannot change a surface without an active fresh game");
}

GameSessionUpdateResult RobloxFreshGameLaunchController::RebindSurface(
    GameSurface surface) {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_ && runtime_ != nullptr
             ? runtime_->RebindSurface(std::move(surface))
             : RejectSurfaceEvent(
                   "cannot rebind a surface without an active fresh game");
}

GameSessionUpdateResult RobloxFreshGameLaunchController::SurfaceDestroyed(
    uint64_t surface_generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_ && runtime_ != nullptr
             ? runtime_->SurfaceDestroyed(surface_generation)
             : RejectSurfaceEvent(
                   "cannot destroy a surface without an active fresh game");
}

bool RobloxFreshGameLaunchController::active() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_;
}

GameSessionSnapshot RobloxFreshGameLaunchController::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return runtime_ != nullptr ? runtime_->Snapshot() : last_snapshot_;
}

}  // namespace runtime
}  // namespace mocktail
