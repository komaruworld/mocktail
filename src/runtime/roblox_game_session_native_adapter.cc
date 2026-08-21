#include "runtime/roblox_game_session_native_adapter.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include "runtime/owned_pthread.h"

namespace mocktail {
namespace runtime {
namespace {

template <typename JavaVm>
auto AttachCurrentThread(JavaVm* java_vm, JNIEnv** environment, int)
    -> decltype(java_vm->AttachCurrentThread(environment, nullptr)) {
  return java_vm->AttachCurrentThread(environment, nullptr);
}

template <typename JavaVm>
auto AttachCurrentThread(JavaVm* java_vm, JNIEnv** environment, long)
    -> decltype(java_vm->AttachCurrentThread(
        reinterpret_cast<void**>(environment), nullptr)) {
  return java_vm->AttachCurrentThread(reinterpret_cast<void**>(environment),
                                      nullptr);
}

constexpr char kSetTaskSchedulerBackgroundModeSymbol[] =
    "Java_com_roblox_engine_jni_NativeGLInterface_"
    "setTaskSchedulerBackgroundMode";
constexpr char kStartGameSymbol[] =
    "Java_com_roblox_engine_jni_NativeGLInterface_"
    "nativeAppBridgeV2StartGameWithParam";
constexpr char kUpdateSurfaceSymbol[] =
    "Java_com_roblox_engine_jni_NativeGLInterface_"
    "nativeAppBridgeV2UpdateSurfaceGameWithPlatformParams";
constexpr char kPauseGameSymbol[] =
    "Java_com_roblox_engine_jni_NativeGLInterface_"
    "nativeAppBridgeV2PauseGame";
constexpr char kResumeGameSymbol[] =
    "Java_com_roblox_engine_jni_NativeGLInterface_"
    "nativeAppBridgeV2ResumeGameWithPlatformParams";
constexpr char kLeaveGameSymbol[] =
    "Java_com_roblox_engine_jni_NativeGLInterface_"
    "nativeAppBridgeV2LeaveGame";
constexpr char kPauseAppSymbol[] =
    "Java_com_roblox_engine_jni_NativeGLInterface_"
    "nativeAppBridgeV2PauseApp";
constexpr char kDestroyAppSymbol[] =
    "Java_com_roblox_engine_jni_NativeGLInterface_"
    "nativeAppBridgeV2DestroyApp";
constexpr char kUpdateSurfaceAppSymbol[] =
    "Java_com_roblox_engine_jni_NativeGLInterface_"
    "nativeAppBridgeV2UpdateSurfaceAppWithPlatformParams";
constexpr char kStartAppSymbol[] =
    "Java_com_roblox_engine_jni_NativeGLInterface_"
    "nativeAppBridgeV2StartAppWithParams";
constexpr char kCallMessagesFromMainThreadSymbol[] =
    "Java_com_roblox_engine_jni_NativeGLInterface_"
    "nativeCallMessagesFromMainThread";
constexpr char kSurfaceCreatedReason[] = "ES.onSurfaceCreated";
constexpr size_t kGuestLifecycleWorkerStackSize = 64ULL * 1024 * 1024;
constexpr int kGuestLifecycleTimeoutMs = 15000;

struct PumpedGuestCall final {
  JniEnvironmentProvider environment;
  NativeAppBridgeV2PauseGameFn operation = nullptr;
  NativeCallMessagesFromMainThreadFn message_pump = nullptr;
  jclass native_gl_class = nullptr;
  const char* operation_name = nullptr;
  JNIEnv* pump_environment = nullptr;
  Status operation_status = Status::Ok();
  Status pump_status = Status::Ok();
  std::atomic<uint64_t> pump_count{0};
};

Status InvalidArgument(std::string message) {
  return Status::Error(StatusCode::kInvalidArgument, std::move(message));
}

Status FailedPrecondition(std::string message) {
  return Status::Error(StatusCode::kFailedPrecondition, std::move(message));
}

Status Unavailable(std::string message) {
  return Status::Error(StatusCode::kUnavailable, std::move(message));
}

Status PlatformError(std::string message) {
  return Status::Error(StatusCode::kPlatformError, std::move(message));
}

template <typename Function>
void ResolveRequired(const RobloxSymbolLookup& lookup, const char* name,
                     Function* destination,
                     std::vector<std::string>* missing_symbols) {
  *destination = reinterpret_cast<Function>(lookup.Find(name));
  if (*destination == nullptr) {
    missing_symbols->emplace_back(name);
  }
}

Status ResultStatus(const GameSessionUpdateResult& result) {
  if (result.ok()) {
    return Status::Ok();
  }
  return result.cause.ok()
             ? Status::Error(StatusCode::kFailedPrecondition, result.message)
             : result.cause;
}

Status CheckGuestCallException(JNIEnv* env, const char* operation_name) {
  if (env == nullptr || env->ExceptionCheck() != JNI_TRUE) {
    return Status::Ok();
  }
  env->ExceptionClear();
  return PlatformError(
      std::string(operation_name != nullptr ? operation_name : "guest call") +
      " raised a JNI exception");
}

void* RunPumpedGuestCall(void* opaque_context) {
  auto* context = static_cast<PumpedGuestCall*>(opaque_context);
  if (context == nullptr || context->operation == nullptr ||
      context->native_gl_class == nullptr) {
    return nullptr;
  }
  JNIEnv* env = nullptr;
  context->operation_status = context->environment.Acquire(&env);
  if (!context->operation_status.ok()) {
    return nullptr;
  }
  context->operation(env, context->native_gl_class);
  context->operation_status =
      CheckGuestCallException(env, context->operation_name);
  return nullptr;
}

void PumpGuestMainThread(void* opaque_context) {
  auto* context = static_cast<PumpedGuestCall*>(opaque_context);
  if (context == nullptr || context->message_pump == nullptr ||
      context->pump_environment == nullptr ||
      context->native_gl_class == nullptr || !context->pump_status.ok()) {
    return;
  }
  context->message_pump(context->pump_environment, context->native_gl_class);
  context->pump_status = CheckGuestCallException(
      context->pump_environment, "pump guest main-thread messages");
  context->pump_count.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

bool RobloxGameSessionSymbols::complete() const {
  return set_task_scheduler_background_mode != nullptr &&
         start_game != nullptr && update_surface != nullptr &&
         pause_game != nullptr && resume_game != nullptr &&
         leave_game != nullptr && pause_app != nullptr &&
         destroy_app != nullptr && update_surface_app != nullptr &&
         start_app != nullptr && call_messages_from_main_thread != nullptr;
}

RobloxAppLifecycleSymbols RobloxGameSessionSymbols::app_lifecycle_symbols()
    const {
  return {pause_app, set_task_scheduler_background_mode, destroy_app};
}

RobloxGameSessionResolution ResolveRobloxGameSessionSymbols(
    const RobloxSymbolLookup& lookup) {
  RobloxGameSessionResolution resolution;
  if (!lookup.valid()) {
    return resolution;
  }

  ResolveRequired(lookup, kSetTaskSchedulerBackgroundModeSymbol,
                  &resolution.symbols_.set_task_scheduler_background_mode,
                  &resolution.missing_required_symbols_);
  ResolveRequired(lookup, kStartGameSymbol, &resolution.symbols_.start_game,
                  &resolution.missing_required_symbols_);
  ResolveRequired(lookup, kUpdateSurfaceSymbol,
                  &resolution.symbols_.update_surface,
                  &resolution.missing_required_symbols_);
  ResolveRequired(lookup, kPauseGameSymbol, &resolution.symbols_.pause_game,
                  &resolution.missing_required_symbols_);
  ResolveRequired(lookup, kResumeGameSymbol, &resolution.symbols_.resume_game,
                  &resolution.missing_required_symbols_);
  ResolveRequired(lookup, kLeaveGameSymbol, &resolution.symbols_.leave_game,
                  &resolution.missing_required_symbols_);
  ResolveRequired(lookup, kPauseAppSymbol, &resolution.symbols_.pause_app,
                  &resolution.missing_required_symbols_);
  ResolveRequired(lookup, kDestroyAppSymbol, &resolution.symbols_.destroy_app,
                  &resolution.missing_required_symbols_);
  ResolveRequired(lookup, kUpdateSurfaceAppSymbol,
                  &resolution.symbols_.update_surface_app,
                  &resolution.missing_required_symbols_);
  ResolveRequired(lookup, kStartAppSymbol, &resolution.symbols_.start_app,
                  &resolution.missing_required_symbols_);
  ResolveRequired(lookup, kCallMessagesFromMainThreadSymbol,
                  &resolution.symbols_.call_messages_from_main_thread,
                  &resolution.missing_required_symbols_);
  resolution.status_ =
      resolution.missing_required_symbols_.empty()
          ? RobloxGameSessionResolutionStatus::kReady
          : RobloxGameSessionResolutionStatus::kMissingRequiredSymbols;
  return resolution;
}

Status JniEnvironmentProvider::Acquire(JNIEnv** env) const {
  if (env == nullptr) {
    return InvalidArgument("JNI environment output is required");
  }
  *env = nullptr;
  if (!valid()) {
    return Unavailable("JavaVM is unavailable");
  }

  JNIEnv* raw_env = nullptr;
  jint result =
      java_vm->GetEnv(reinterpret_cast<void**>(&raw_env), JNI_VERSION_1_6);
  if (result == JNI_EDETACHED) {
    result = AttachCurrentThread(java_vm, &raw_env, 0);
  }
  if (result != JNI_OK || raw_env == nullptr) {
    return Unavailable("current thread could not acquire JNIEnv");
  }
  if (prepare != nullptr) {
    prepare(prepare_context);
  }
  *env = raw_env;
  return Status::Ok();
}

RobloxGameSessionNativeAdapter::RobloxGameSessionNativeAdapter(
    JniEnvironmentProvider environment, RobloxGameSessionSymbols symbols,
    RobloxGameSurfaceJniConfig surface_config)
    : environment_(environment),
      symbols_(symbols),
      surface_config_(std::move(surface_config)),
      app_lifecycle_(symbols.app_lifecycle_symbols()) {}

RobloxGameSessionNativeAdapter::~RobloxGameSessionNativeAdapter() { Release(); }

Status RobloxGameSessionNativeAdapter::Initialize(
    const RobloxGameSessionBinding& binding) {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  if (initialized_) {
    return FailedPrecondition(
        "game-session JNI adapter is already initialized");
  }
  if (!environment_.valid()) {
    return Unavailable("game-session JNI environment provider is invalid");
  }
  if (!symbols_.complete()) {
    return Unavailable("game-session native symbol table is incomplete");
  }
  if (!surface_config_.complete()) {
    return InvalidArgument("game-session surface JNI config is incomplete");
  }
  if (!binding.complete()) {
    return InvalidArgument("game-session JNI binding is incomplete");
  }

  JNIEnv* env = nullptr;
  Status status = environment_.Acquire(&env);
  if (!status.ok()) {
    return status;
  }

  RobloxGameSessionJniObjects retained;
  retained.native_gl_class =
      static_cast<jclass>(env->NewGlobalRef(binding.objects.native_gl_class));
  retained.surface = env->NewGlobalRef(binding.objects.surface);
  retained.platform_params = env->NewGlobalRef(binding.objects.platform_params);
  retained.activity = binding.objects.activity != nullptr
                          ? env->NewGlobalRef(binding.objects.activity)
                          : nullptr;
  retained.start_game_params =
      env->NewGlobalRef(binding.objects.start_game_params);
  if (!retained.complete()) {
    ReleaseGlobalReferenceLocked(env, retained.start_game_params);
    ReleaseGlobalReferenceLocked(env, retained.activity);
    ReleaseGlobalReferenceLocked(env, retained.platform_params);
    ReleaseGlobalReferenceLocked(env, retained.surface);
    ReleaseGlobalReferenceLocked(env, retained.native_gl_class);
    return Unavailable("could not retain game-session JNI global references");
  }
  status = CheckJniException(env, "retain game-session JNI references");
  if (!status.ok()) {
    ReleaseGlobalReferenceLocked(env, retained.start_game_params);
    ReleaseGlobalReferenceLocked(env, retained.activity);
    ReleaseGlobalReferenceLocked(env, retained.platform_params);
    ReleaseGlobalReferenceLocked(env, retained.surface);
    ReleaseGlobalReferenceLocked(env, retained.native_gl_class);
    return status;
  }
  binding_ = binding;
  binding_.objects = retained;
  initialized_ = true;
  return Status::Ok();
}

Status RobloxGameSessionNativeAdapter::Release() {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  if (!initialized_ && !binding_.objects.complete()) {
    return Status::Ok();
  }
  JNIEnv* env = nullptr;
  Status status = environment_.Acquire(&env);
  if (!status.ok()) {
    return status;
  }
  ReleaseGlobalReferencesLocked(env);
  binding_ = {};
  retained_surface_ = {};
  initialized_ = false;
  return CheckJniException(env, "release game-session JNI references");
}

bool RobloxGameSessionNativeAdapter::initialized() const {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  return initialized_;
}

GameSessionCapabilities RobloxGameSessionNativeAdapter::Capabilities() {
  return {this,
          &RobloxGameSessionNativeAdapter::SetForegroundCallback,
          &RobloxGameSessionNativeAdapter::StartCallback,
          &RobloxGameSessionNativeAdapter::UpdateCallback,
          &RobloxGameSessionNativeAdapter::PauseCallback,
          &RobloxGameSessionNativeAdapter::ResumeCallback,
          &RobloxGameSessionNativeAdapter::LeaveCallback,
          &RobloxGameSessionNativeAdapter::ShutdownCallback};
}

Status RobloxGameSessionNativeAdapter::SetForegroundCallback(void* context) {
  return static_cast<RobloxGameSessionNativeAdapter*>(context)->SetForeground();
}

Status RobloxGameSessionNativeAdapter::StartCallback(
    void* context, const GameSessionPrincipal& principal,
    const GameJoinRequest& request, const GameSurface& surface) {
  return static_cast<RobloxGameSessionNativeAdapter*>(context)->Start(
      principal, request, surface);
}

Status RobloxGameSessionNativeAdapter::UpdateCallback(
    void* context, const GameSurface& surface) {
  return static_cast<RobloxGameSessionNativeAdapter*>(context)->Update(surface);
}

Status RobloxGameSessionNativeAdapter::PauseCallback(void* context) {
  return static_cast<RobloxGameSessionNativeAdapter*>(context)->Pause();
}

Status RobloxGameSessionNativeAdapter::ResumeCallback(
    void* context, const GameSurface& surface) {
  return static_cast<RobloxGameSessionNativeAdapter*>(context)->Resume(surface);
}

Status RobloxGameSessionNativeAdapter::LeaveCallback(void* context) {
  return static_cast<RobloxGameSessionNativeAdapter*>(context)->Leave();
}

Status RobloxGameSessionNativeAdapter::ShutdownCallback(void* context) {
  return static_cast<RobloxGameSessionNativeAdapter*>(context)->Shutdown();
}

Status RobloxGameSessionNativeAdapter::SetForeground() {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  if (!initialized_) {
    return FailedPrecondition("game-session JNI adapter is not initialized");
  }
  JNIEnv* env = nullptr;
  Status status = environment_.Acquire(&env);
  if (!status.ok()) {
    return status;
  }
  jstring reason = env->NewStringUTF(kSurfaceCreatedReason);
  if (reason == nullptr) {
    return Unavailable("could not allocate SurfaceCreated reason");
  }
  std::fprintf(stderr,
               "  [game-session] setTaskSchedulerBackgroundMode(false, "
               "ES.onSurfaceCreated)\n");
  symbols_.set_task_scheduler_background_mode(
      env, binding_.objects.native_gl_class, JNI_FALSE, reason);
  env->DeleteLocalRef(reason);
  return CheckJniException(env, "foreground game surface");
}

Status RobloxGameSessionNativeAdapter::Start(
    const GameSessionPrincipal& principal, const GameJoinRequest& request,
    const GameSurface& surface) {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  if (!initialized_) {
    return FailedPrecondition("game-session JNI adapter is not initialized");
  }
  if (principal.kind != binding_.principal.kind ||
      principal.generation != binding_.principal.generation ||
      principal.principal_id != binding_.principal.principal_id ||
      principal.base_url != binding_.principal.base_url ||
      request.request_id != binding_.request.request_id ||
      request.place_id != binding_.request.place_id ||
      request.launch_parameters != binding_.request.launch_parameters ||
      surface.generation != binding_.surface.generation ||
      surface.native_window != binding_.surface.native_window ||
      surface.width != binding_.surface.width ||
      surface.height != binding_.surface.height) {
    return FailedPrecondition(
        "coordinator start does not match retained StartGameParams");
  }
  JNIEnv* env = nullptr;
  Status status = environment_.Acquire(&env);
  if (!status.ok()) {
    return status;
  }
  std::fprintf(stderr, "  [engine] nativeAppBridgeV2StartGameWithParam\n");
  const jint result = symbols_.start_game(env, binding_.objects.native_gl_class,
                                          binding_.objects.start_game_params);
  std::fprintf(stderr,
               "  [engine] nativeAppBridgeV2StartGameWithParam returned %d\n",
               result);
  status = CheckJniException(env, "start game");
  if (status.ok()) {
    retained_surface_ = surface;
  }
  return status;
}

Status RobloxGameSessionNativeAdapter::Update(const GameSurface& surface) {
  return RebindAndInvoke(surface, false);
}

Status RobloxGameSessionNativeAdapter::Pause() {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  if (!initialized_) {
    return FailedPrecondition("game-session JNI adapter is not initialized");
  }
  return InvokeWithMainThreadPump(symbols_.pause_game, "pause game");
}

Status RobloxGameSessionNativeAdapter::Resume(const GameSurface& surface) {
  return RebindAndInvoke(surface, true);
}

Status RobloxGameSessionNativeAdapter::RebindAndInvoke(
    const GameSurface& surface, bool resume) {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  if (!initialized_ || surface.generation == 0 || surface.native_window == 0 ||
      surface.width == 0 || surface.height == 0) {
    return FailedPrecondition(resume
                                  ? "game surface resume is not initialized"
                                  : "game surface update is not initialized");
  }
  if (retained_surface_.generation == 0) {
    return FailedPrecondition("initial game surface was not retained");
  }
  const bool replaces_surface =
      surface.generation != retained_surface_.generation;
  if (!replaces_surface &&
      surface.native_window != retained_surface_.native_window) {
    return FailedPrecondition(
        "same-generation game surface changed its native window");
  }

  JNIEnv* env = nullptr;
  Status status = environment_.Acquire(&env);
  if (!status.ok()) {
    return status;
  }

  jobject candidate_surface = binding_.objects.surface;
  jobject candidate_surface_local = nullptr;
  if (replaces_surface) {
    status = CreateSurfaceObjectLocked(env, surface, &candidate_surface_local);
    if (!status.ok()) {
      return status;
    }
    candidate_surface = env->NewGlobalRef(candidate_surface_local);
    env->DeleteLocalRef(candidate_surface_local);
    if (candidate_surface == nullptr) {
      CheckJniException(env, "retain recreated game surface");
      return Unavailable("could not retain recreated game surface");
    }
  }

  jobject candidate_params_local = nullptr;
  status = CreatePlatformParamsLocked(env, &candidate_params_local);
  if (!status.ok()) {
    if (replaces_surface) {
      ReleaseGlobalReferenceLocked(env, candidate_surface);
    }
    return status;
  }
  jobject candidate_params = env->NewGlobalRef(candidate_params_local);
  env->DeleteLocalRef(candidate_params_local);
  if (candidate_params == nullptr) {
    if (replaces_surface) {
      ReleaseGlobalReferenceLocked(env, candidate_surface);
    }
    CheckJniException(env, "retain game platform params");
    return Unavailable("could not retain game platform params");
  }

  if (resume) {
    symbols_.resume_game(env, binding_.objects.native_gl_class,
                         candidate_surface, candidate_params,
                         binding_.objects.activity);
  } else {
    symbols_.update_surface(env, binding_.objects.native_gl_class,
                            candidate_surface, candidate_params,
                            binding_.objects.activity);
  }
  status =
      CheckJniException(env, resume ? "resume game" : "update game surface");
  if (!status.ok()) {
    ReleaseGlobalReferenceLocked(env, candidate_params);
    if (replaces_surface) {
      ReleaseGlobalReferenceLocked(env, candidate_surface);
    }
    return status;
  }

  jobject old_params = binding_.objects.platform_params;
  jobject old_surface = binding_.objects.surface;
  binding_.objects.platform_params = candidate_params;
  if (replaces_surface) {
    binding_.objects.surface = candidate_surface;
  }
  retained_surface_ = surface;
  ReleaseGlobalReferenceLocked(env, old_params);
  if (replaces_surface) {
    ReleaseGlobalReferenceLocked(env, old_surface);
  }
  return CheckJniException(env, resume ? "commit resumed game surface"
                                       : "commit updated game surface");
}

Status RobloxGameSessionNativeAdapter::Leave() {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  if (!initialized_) {
    return FailedPrecondition("game-session JNI adapter is not initialized");
  }
  return InvokeWithMainThreadPump(symbols_.leave_game, "leave game");
}

Status RobloxGameSessionNativeAdapter::InvokeWithMainThreadPump(
    NativeAppBridgeV2PauseGameFn operation, const char* operation_name) {
  if (operation == nullptr ||
      symbols_.call_messages_from_main_thread == nullptr) {
    return Unavailable(
        "guest lifecycle operation or main-thread pump is "
        "unavailable");
  }

  PumpedGuestCall context;
  context.environment = environment_;
  context.operation = operation;
  context.message_pump = symbols_.call_messages_from_main_thread;
  context.native_gl_class = binding_.objects.native_gl_class;
  context.operation_name = operation_name;
  Status status = environment_.Acquire(&context.pump_environment);
  if (!status.ok()) {
    return status;
  }

  OwnedPthread worker;
  const int start_result = worker.Start(&RunPumpedGuestCall, &context,
                                        kGuestLifecycleWorkerStackSize);
  if (start_result != 0) {
    return PlatformError(std::string("could not start ") + operation_name +
                         " lifecycle worker: " + std::to_string(start_result));
  }
  std::fprintf(stderr,
               "  [game-session] %s dispatched; pumping main-thread "
               "messages\n",
               operation_name);
  const OwnedPthreadWaitResult wait = worker.WaitFor(
      kGuestLifecycleTimeoutMs, 1, &PumpGuestMainThread, &context);
  if (!wait.joined()) {
    std::fprintf(stderr,
                 "[FATAL] %s did not finish after %d ms; terminating instead "
                 "of hanging with live guest workers\n",
                 operation_name, kGuestLifecycleTimeoutMs);
    std::fflush(stderr);
    std::_Exit(EXIT_FAILURE);
  }
  std::fprintf(stderr,
               "  [game-session] %s returned after %llu main-thread pumps\n",
               operation_name,
               static_cast<unsigned long long>(
                   context.pump_count.load(std::memory_order_relaxed)));
  return context.operation_status.ok() ? context.pump_status
                                       : context.operation_status;
}

Status RobloxGameSessionNativeAdapter::Shutdown() {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  if (!initialized_) {
    return FailedPrecondition("game-session JNI adapter is not initialized");
  }
  JNIEnv* env = nullptr;
  Status status = environment_.Acquire(&env);
  if (!status.ok()) {
    return status;
  }
  const RobloxAppShutdownResult shutdown =
      app_lifecycle_.Shutdown(env, binding_.objects.native_gl_class);
  if (!shutdown.ok()) {
    return PlatformError(shutdown.message);
  }
  status = CheckJniException(env, "shutdown game app runtime");
  if (!status.ok()) {
    return status;
  }
  ReleaseGlobalReferencesLocked(env);
  binding_ = {};
  retained_surface_ = {};
  initialized_ = false;
  return Status::Ok();
}

Status RobloxGameSessionNativeAdapter::CreateSurfaceObjectLocked(
    JNIEnv* env, const GameSurface& surface, jobject* result) const {
  if (env == nullptr || result == nullptr) {
    return InvalidArgument("surface JNI output is required");
  }
  *result = nullptr;
  jclass surface_class = env->FindClass("android/view/Surface");
  if (surface_class == nullptr) {
    return Unavailable("android.view.Surface is unavailable");
  }
  jobject object = env->AllocObject(surface_class);
  if (object == nullptr) {
    env->DeleteLocalRef(surface_class);
    return Unavailable("could not allocate android.view.Surface");
  }

  const auto set_int = [env, surface_class, object](const char* name,
                                                    jint value) {
    jfieldID field = env->GetFieldID(surface_class, name, "I");
    if (field == nullptr) {
      return false;
    }
    env->SetIntField(object, field, value);
    return true;
  };
  const auto set_boolean = [env, surface_class, object](const char* name,
                                                        jboolean value) {
    jfieldID field = env->GetFieldID(surface_class, name, "Z");
    if (field == nullptr) {
      return false;
    }
    env->SetBooleanField(object, field, value);
    return true;
  };
  const bool populated = set_int("width", static_cast<jint>(surface.width)) &&
                         set_int("height", static_cast<jint>(surface.height)) &&
                         set_boolean("valid", JNI_TRUE) &&
                         set_boolean("isValid", JNI_TRUE);
  env->DeleteLocalRef(surface_class);
  if (!populated) {
    env->DeleteLocalRef(object);
    CheckJniException(env, "populate recreated android surface");
    return Unavailable("recreated android surface fields are unavailable");
  }
  Status status = CheckJniException(env, "create recreated android surface");
  if (!status.ok()) {
    env->DeleteLocalRef(object);
    return status;
  }
  *result = object;
  return Status::Ok();
}

Status RobloxGameSessionNativeAdapter::CreatePlatformParamsLocked(
    JNIEnv* env, jobject* result) const {
  if (env == nullptr || result == nullptr) {
    return InvalidArgument("platform params JNI output is required");
  }
  *result = nullptr;
  jclass params_class =
      env->FindClass("com/roblox/engine/jni/model/PlatformParams");
  if (params_class == nullptr) {
    return Unavailable("Roblox PlatformParams is unavailable");
  }
  jobject object = env->AllocObject(params_class);
  if (object == nullptr) {
    env->DeleteLocalRef(params_class);
    return Unavailable("could not allocate Roblox PlatformParams");
  }

  const auto set_boolean = [env, params_class, object](const char* name,
                                                       jboolean value) {
    jfieldID field = env->GetFieldID(params_class, name, "Z");
    if (field == nullptr) {
      return false;
    }
    env->SetBooleanField(object, field, value);
    return true;
  };
  const auto set_int = [env, params_class, object](const char* name,
                                                   jint value) {
    jfieldID field = env->GetFieldID(params_class, name, "I");
    if (field == nullptr) {
      return false;
    }
    env->SetIntField(object, field, value);
    return true;
  };
  jfieldID dpi_scale = env->GetFieldID(params_class, "dpiScale", "F");
  jfieldID asset_folder =
      env->GetFieldID(params_class, "assetFolderPath", "Ljava/lang/String;");
  jstring asset_path =
      env->NewStringUTF(surface_config_.asset_folder_path.c_str());
  const bool populated =
      dpi_scale != nullptr && asset_folder != nullptr &&
      asset_path != nullptr &&
      set_boolean("isTouchDevice",
                  surface_config_.is_touch_device ? JNI_TRUE : JNI_FALSE) &&
      set_boolean("isMouseDevice",
                  surface_config_.is_mouse_device ? JNI_TRUE : JNI_FALSE) &&
      set_boolean("isKeyboardDevice",
                  surface_config_.is_keyboard_device ? JNI_TRUE : JNI_FALSE) &&
      set_int("viewportWidthMm", surface_config_.viewport_width_mm) &&
      set_int("viewportHeightMm", surface_config_.viewport_height_mm);
  if (populated) {
    env->SetFloatField(object, dpi_scale, surface_config_.dpi_scale);
    env->SetObjectField(object, asset_folder, asset_path);
  }
  if (asset_path != nullptr) {
    env->DeleteLocalRef(asset_path);
  }
  env->DeleteLocalRef(params_class);
  if (!populated) {
    env->DeleteLocalRef(object);
    CheckJniException(env, "populate Roblox PlatformParams");
    return Unavailable("required Roblox PlatformParams fields are unavailable");
  }
  Status status = CheckJniException(env, "create Roblox PlatformParams");
  if (!status.ok()) {
    env->DeleteLocalRef(object);
    return status;
  }
  *result = object;
  return Status::Ok();
}

Status RobloxGameSessionNativeAdapter::CheckJniException(
    JNIEnv* env, const char* operation) const {
  if (env == nullptr) {
    return Unavailable("JNIEnv disappeared during native operation");
  }
  if (env->ExceptionCheck() == JNI_FALSE) {
    return Status::Ok();
  }
  env->ExceptionClear();
  return PlatformError(
      std::string(operation != nullptr ? operation : "native operation") +
      " raised a JNI exception");
}

void RobloxGameSessionNativeAdapter::ReleaseGlobalReferencesLocked(
    JNIEnv* env) {
  if (env == nullptr) {
    return;
  }
  ReleaseGlobalReferenceLocked(env, binding_.objects.start_game_params);
  ReleaseGlobalReferenceLocked(env, binding_.objects.activity);
  ReleaseGlobalReferenceLocked(env, binding_.objects.platform_params);
  ReleaseGlobalReferenceLocked(env, binding_.objects.surface);
  ReleaseGlobalReferenceLocked(env, binding_.objects.native_gl_class);
  binding_.objects = {};
}

void RobloxGameSessionNativeAdapter::ReleaseGlobalReferenceLocked(
    JNIEnv* env, jobject reference) {
  if (env != nullptr && reference != nullptr) {
    env->DeleteGlobalRef(reference);
  }
}

RobloxGameSessionRuntime::RobloxGameSessionRuntime(
    JniEnvironmentProvider environment, RobloxGameSessionSymbols symbols,
    RobloxGameSurfaceJniConfig surface_config,
    RobloxGamePresentedObserver presented_observer)
    : adapter_(environment, symbols, std::move(surface_config)),
      coordinator_(adapter_.Capabilities()),
      presented_observer_(presented_observer) {}

Status RobloxGameSessionRuntime::InitializeAndStart(
    const RobloxGameSessionBinding& binding, GameSessionPrincipal principal,
    GameJoinRequest request, GameSurface surface) {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  present_evidence_recorded_.store(false, std::memory_order_release);
  Status status = adapter_.Initialize(binding);
  if (status.ok()) {
    status = ResultStatus(coordinator_.SetPrincipal(std::move(principal)));
  }
  if (status.ok()) {
    status = ResultStatus(coordinator_.QueueJoinRequest(std::move(request)));
  }
  if (status.ok()) {
    status = ResultStatus(coordinator_.SurfaceCreated(surface.generation));
  }
  if (status.ok()) {
    status = ResultStatus(coordinator_.SurfaceChanged(surface));
  }
  {
    std::lock_guard<std::mutex> lock(status_mutex_);
    startup_status_ = status;
  }
  return status;
}

GameSessionUpdateResult RobloxGameSessionRuntime::RecordPresent(
    uint64_t frame_serial) {
  const GameSessionSnapshot snapshot = coordinator_.Snapshot();
  const uint64_t generation = snapshot.pending_surface_generation != 0
                                  ? snapshot.pending_surface_generation
                                  : snapshot.active_surface_generation;
  return coordinator_.RecordGamePresent(
      {generation, frame_serial, GamePresentBackend::kVulkan, true});
}

GameSessionUpdateResult RobloxGameSessionRuntime::SurfaceCreated(
    uint64_t surface_generation) {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  return coordinator_.SurfaceCreated(surface_generation);
}

GameSessionUpdateResult RobloxGameSessionRuntime::SurfaceChanged(
    GameSurface surface) {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  return coordinator_.SurfaceChanged(surface);
}

GameSessionUpdateResult RobloxGameSessionRuntime::RebindSurface(
    GameSurface surface) {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  return coordinator_.RebindSurface(surface);
}

GameSessionUpdateResult RobloxGameSessionRuntime::SurfaceDestroyed(
    uint64_t surface_generation) {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  return coordinator_.SurfaceDestroyed(surface_generation);
}

GameSessionUpdateResult RobloxGameSessionRuntime::LeaveGame() {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  return coordinator_.LeaveGame();
}

GameSessionUpdateResult RobloxGameSessionRuntime::Shutdown() {
  std::lock_guard<std::mutex> lifecycle_lock(lifecycle_mutex_);
  GameSessionSnapshot snapshot = coordinator_.Snapshot();
  if (snapshot.state == GameSessionState::kRunning &&
      snapshot.active_surface_available) {
    std::fprintf(stderr,
                 "  [game-session] graceful shutdown: pausing the active "
                 "UGCGame surface\n");
    const GameSessionUpdateResult pause =
        coordinator_.SurfaceDestroyed(snapshot.active_surface_generation);
    if (!pause.ok()) {
      return pause;
    }
    snapshot = coordinator_.Snapshot();
  }
  if (snapshot.state == GameSessionState::kRunning ||
      snapshot.state == GameSessionState::kPaused) {
    std::fprintf(stderr,
                 "  [game-session] graceful shutdown: leaving UGCGame "
                 "before app teardown\n");
    const GameSessionUpdateResult leave = coordinator_.LeaveGame();
    if (!leave.ok()) {
      return leave;
    }
  }
  return coordinator_.Shutdown();
}

GameSessionSnapshot RobloxGameSessionRuntime::Snapshot() const {
  return coordinator_.Snapshot();
}

Status RobloxGameSessionRuntime::startup_status() const {
  std::lock_guard<std::mutex> lock(status_mutex_);
  return startup_status_;
}

void RobloxGameSessionRuntime::SuccessfulPresentCallback(
    void* context, uint64_t frame_serial) {
  if (context == nullptr || frame_serial == 0) {
    return;
  }
  auto* runtime = static_cast<RobloxGameSessionRuntime*>(context);
  if (runtime->present_evidence_recorded_.load(std::memory_order_acquire)) {
    return;
  }
  const GameSessionUpdateResult result = runtime->RecordPresent(frame_serial);
  if (result.changed()) {
    runtime->present_evidence_recorded_.store(true, std::memory_order_release);
  }
  if (result.changed() && runtime->presented_observer_.valid()) {
    runtime->presented_observer_.notify(runtime->presented_observer_.context,
                                        frame_serial);
  }
  if (result.changed()) {
    std::fprintf(stderr, "  [game-session] host present accepted frame=%llu\n",
                 static_cast<unsigned long long>(frame_serial));
  }
}

const char* RobloxGameSessionResolutionStatusName(
    RobloxGameSessionResolutionStatus status) {
  switch (status) {
    case RobloxGameSessionResolutionStatus::kReady:
      return "Ready";
    case RobloxGameSessionResolutionStatus::kInvalidLookup:
      return "InvalidLookup";
    case RobloxGameSessionResolutionStatus::kMissingRequiredSymbols:
      return "MissingRequiredSymbols";
  }
  return "Unknown";
}

}  // namespace runtime
}  // namespace mocktail
