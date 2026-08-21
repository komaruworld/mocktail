#include "runtime/roblox_app_lifecycle.h"

#include <utility>

namespace mocktail {
namespace runtime {
namespace {

constexpr char kPauseAppSymbol[] =
    "Java_com_roblox_engine_jni_NativeGLInterface_"
    "nativeAppBridgeV2PauseApp";
constexpr char kBackgroundModeSymbol[] =
    "Java_com_roblox_engine_jni_NativeGLInterface_"
    "setTaskSchedulerBackgroundMode";
constexpr char kDestroyAppSymbol[] =
    "Java_com_roblox_engine_jni_NativeGLInterface_"
    "nativeAppBridgeV2DestroyApp";
constexpr char kShutdownContext[] = "ASMA.stop";

template <typename Function>
void ResolveRequired(const RobloxSymbolLookup &lookup, const char *symbol_name,
                     Function *destination,
                     std::vector<std::string> *missing_symbols) {
  *destination = reinterpret_cast<Function>(lookup.Find(symbol_name));
  if (*destination == nullptr) {
    missing_symbols->emplace_back(symbol_name);
  }
}

} // namespace

bool RobloxAppLifecycleSymbols::complete() const {
  return app_bridge_v2_pause_app != nullptr &&
         set_task_scheduler_background_mode != nullptr &&
         app_bridge_v2_destroy_app != nullptr;
}

RobloxAppLifecycleResolution
ResolveRobloxAppLifecycleSymbols(const RobloxSymbolLookup &lookup) {
  RobloxAppLifecycleResolution resolution;
  if (!lookup.valid()) {
    return resolution;
  }

  ResolveRequired(lookup, kPauseAppSymbol,
                  &resolution.symbols_.app_bridge_v2_pause_app,
                  &resolution.missing_required_symbols_);
  ResolveRequired(lookup, kBackgroundModeSymbol,
                  &resolution.symbols_.set_task_scheduler_background_mode,
                  &resolution.missing_required_symbols_);
  ResolveRequired(lookup, kDestroyAppSymbol,
                  &resolution.symbols_.app_bridge_v2_destroy_app,
                  &resolution.missing_required_symbols_);
  resolution.status_ =
      resolution.missing_required_symbols_.empty()
          ? RobloxAppLifecycleResolutionStatus::kReady
          : RobloxAppLifecycleResolutionStatus::kMissingRequiredSymbols;
  return resolution;
}

RobloxAppLifecycle::RobloxAppLifecycle(RobloxAppLifecycleSymbols symbols)
    : symbols_(std::move(symbols)) {}

RobloxAppShutdownResult RobloxAppLifecycle::Shutdown(JNIEnv *env,
                                                     jclass native_gl_class) {
  if (!symbols_.complete()) {
    return {RobloxAppShutdownStatus::kMissingRequiredSymbols,
            "lifecycle symbols are incomplete"};
  }
  if (env == nullptr || native_gl_class == nullptr) {
    return {RobloxAppShutdownStatus::kInvalidJniState,
            "JNI environment and NativeGLInterface class are required"};
  }

  const State current = state_.load(std::memory_order_acquire);
  if (current == State::kStopped) {
    return {RobloxAppShutdownStatus::kAlreadyStopped,
            "Roblox app is already stopped"};
  }
  if (current == State::kStopping) {
    return {RobloxAppShutdownStatus::kShutdownInProgress,
            "Roblox app shutdown is already in progress"};
  }

  jstring context = env->NewStringUTF(kShutdownContext);
  if (context == nullptr) {
    return {RobloxAppShutdownStatus::kReasonAllocationFailed,
            "could not allocate ASMA.stop JNI string"};
  }

  State expected = State::kReady;
  if (!state_.compare_exchange_strong(expected, State::kStopping,
                                      std::memory_order_acq_rel)) {
    env->DeleteLocalRef(context);
    if (expected == State::kStopped) {
      return {RobloxAppShutdownStatus::kAlreadyStopped,
              "Roblox app is already stopped"};
    }
    return {RobloxAppShutdownStatus::kShutdownInProgress,
            "Roblox app shutdown is already in progress"};
  }

  symbols_.app_bridge_v2_pause_app(env, native_gl_class);
  symbols_.set_task_scheduler_background_mode(env, native_gl_class, JNI_TRUE,
                                              context);
  symbols_.app_bridge_v2_destroy_app(env, native_gl_class);
  env->DeleteLocalRef(context);
  state_.store(State::kStopped, std::memory_order_release);
  return {RobloxAppShutdownStatus::kStopped,
          "Roblox app lifecycle shutdown completed"};
}

bool RobloxAppLifecycle::stopped() const {
  return state_.load(std::memory_order_acquire) == State::kStopped;
}

const char *RobloxAppLifecycleResolutionStatusName(
    RobloxAppLifecycleResolutionStatus status) {
  switch (status) {
  case RobloxAppLifecycleResolutionStatus::kReady:
    return "Ready";
  case RobloxAppLifecycleResolutionStatus::kInvalidLookup:
    return "InvalidLookup";
  case RobloxAppLifecycleResolutionStatus::kMissingRequiredSymbols:
    return "MissingRequiredSymbols";
  }
  return "Unknown";
}

const char *RobloxAppShutdownStatusName(RobloxAppShutdownStatus status) {
  switch (status) {
  case RobloxAppShutdownStatus::kStopped:
    return "Stopped";
  case RobloxAppShutdownStatus::kAlreadyStopped:
    return "AlreadyStopped";
  case RobloxAppShutdownStatus::kShutdownInProgress:
    return "ShutdownInProgress";
  case RobloxAppShutdownStatus::kMissingRequiredSymbols:
    return "MissingRequiredSymbols";
  case RobloxAppShutdownStatus::kInvalidJniState:
    return "InvalidJniState";
  case RobloxAppShutdownStatus::kReasonAllocationFailed:
    return "ReasonAllocationFailed";
  }
  return "Unknown";
}

} // namespace runtime
} // namespace mocktail
