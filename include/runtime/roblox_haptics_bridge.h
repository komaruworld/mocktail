#ifndef MOCKTAIL_RUNTIME_ROBLOX_HAPTICS_BRIDGE_H_
#define MOCKTAIL_RUNTIME_ROBLOX_HAPTICS_BRIDGE_H_

#include <jni.h>

#include <memory>
#include <mutex>

#include "runtime/roblox_haptics.h"
#include "runtime/roblox_message_bus_request_bridge.h"

namespace mocktail {
namespace runtime {

using UpdateMobileHapticsStateFn = void (*)(JNIEnv*, jclass, jboolean, jstring);

class RobloxHapticsBridge final {
 public:
  RobloxHapticsBridge(JniEnvironmentProvider environment,
                      RobloxMessageBusSymbols symbols,
                      RobloxMessageBusObjects objects,
                      std::shared_ptr<RobloxHaptics> haptics,
                      UpdateMobileHapticsStateFn update_state);
  Status Initialize();
  Status Shutdown();
  void RefreshCapabilities(jclass native_gl_class);

 private:
  const JniEnvironmentProvider environment_;
  const std::shared_ptr<RobloxHaptics> haptics_;
  const UpdateMobileHapticsStateFn update_state_;
  RobloxMessageBusRequestBridge bridge_;
  std::mutex capability_mutex_;
  bool notified_ = false;
  bool last_available_ = false;
};

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_ROBLOX_HAPTICS_BRIDGE_H_
