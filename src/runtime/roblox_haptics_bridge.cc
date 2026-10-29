#include "runtime/roblox_haptics_bridge.h"

#include <array>
#include <iterator>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

namespace mocktail {
namespace runtime {
namespace {

constexpr char kProtocol[] = "HapticProtocol";
constexpr const char* kMethods[] = {"SupportsHaptics",
                                    "UpdateSingletonVibration", "GetMotors",
                                    "SetMotorState"};
constexpr char kMotorsJson[] = R"({"defaultMotor":{"x":0,"y":0,"z":0}})";
constexpr std::size_t kMaximumJsonBytes = 256 * 1024;

RobloxMessageBusReply Answer(JNIEnv* env, jstring message, std::size_t method,
                             const std::shared_ptr<RobloxHaptics>& haptics) {
  if (method == 0) {
    const bool available = haptics->available();
    return {nlohmann::json{{"supportsHaptics", available}}.dump(), 0,
            available ? "supported" : "unavailable"};
  }
  if (method == 2) {
    if (!haptics->available())
      return {"{}", 0, "unavailable"};
    return {kMotorsJson, 0, "motors"};
  }
  std::string owned;
  if (message != nullptr && env->GetStringUTFLength(message) <=
                                static_cast<jsize>(kMaximumJsonBytes)) {
    const char* bytes = env->GetStringUTFChars(message, nullptr);
    if (bytes != nullptr) {
      owned = bytes;
      env->ReleaseStringUTFChars(message, bytes);
    }
  }
  const bool accepted = haptics->Submit(kMethods[method], owned);
  return {"{}", accepted ? 0 : 13, accepted ? "ok" : "invalid_request",
          false};
}

}  // namespace

RobloxHapticsBridge::RobloxHapticsBridge(
    JniEnvironmentProvider environment, RobloxMessageBusSymbols symbols,
    RobloxMessageBusObjects objects, std::shared_ptr<RobloxHaptics> haptics,
    UpdateMobileHapticsStateFn update_state)
    : environment_(environment),
      haptics_(std::move(haptics)),
      update_state_(update_state),
      bridge_(environment, symbols, std::move(objects), kProtocol,
              {std::begin(kMethods), std::end(kMethods)},
              [haptics = haptics_](JNIEnv* env, jstring message,
                                   std::size_t method) {
                return Answer(env, message, method, haptics);
              }) {}

Status RobloxHapticsBridge::Initialize() {
  return bridge_.Initialize();
}

Status RobloxHapticsBridge::Shutdown() {
  std::lock_guard<std::mutex> lock(capability_mutex_);
  notified_ = false;
  haptics_->Cancel();
  return bridge_.Shutdown();
}

void RobloxHapticsBridge::RefreshCapabilities(jclass native_gl_class) {
  std::lock_guard<std::mutex> lock(capability_mutex_);
  if (update_state_ == nullptr || native_gl_class == nullptr ||
      !environment_.valid())
    return;
  const bool available = haptics_->available();
  if (notified_ && available == last_available_)
    return;
  JNIEnv* env = nullptr;
  if (!environment_.Acquire(&env).ok())
    return;
  jstring motors = env->NewStringUTF(available ? kMotorsJson : "{}");
  if (motors != nullptr) {
    update_state_(env, native_gl_class, available ? JNI_TRUE : JNI_FALSE,
                  motors);
    if (!env->ExceptionCheck()) {
      notified_ = true;
      last_available_ = available;
    }
    env->DeleteLocalRef(motors);
  }
  if (env->ExceptionCheck())
    env->ExceptionClear();
}

}  // namespace runtime
}  // namespace mocktail
