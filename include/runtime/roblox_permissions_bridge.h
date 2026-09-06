#ifndef MOCKTAIL_RUNTIME_ROBLOX_PERMISSIONS_BRIDGE_H_
#define MOCKTAIL_RUNTIME_ROBLOX_PERMISSIONS_BRIDGE_H_

#include <jni.h>

#include <memory>
#include <mutex>

#include "mocktail/status.h"
#include "runtime/roblox_game_session_native_adapter.h"

namespace mocktail {
namespace runtime {

// Exact MessageBus JNI entrypoints used by the APK PermissionsProtocol.
struct RobloxPermissionsMessageBusSymbols {
  jobject (*subscribe_request)(JNIEnv*, jobject, jstring, jstring, jobject,
                               jboolean) = nullptr;
  void (*delete_connection)(JNIEnv*, jobject, jlong) = nullptr;
  void (*publish_response)(JNIEnv*, jobject, jstring, jstring, jstring, jint,
                           jstring) = nullptr;
  void (*set_async_handler)(JNIEnv*, jobject, jstring, jstring,
                            jobject) = nullptr;
  void (*clear_handler)(JNIEnv*, jobject, jstring, jstring) = nullptr;
  void (*call_response_handler)(JNIEnv*, jobject, jstring, jstring) = nullptr;

  bool complete() const {
    return subscribe_request && delete_connection && publish_response &&
           set_async_handler && clear_handler && call_response_handler;
  }
};

struct RobloxPermissionsMessageBusObjects {
  jobject message_bus = nullptr;
  void* context = nullptr;
  jobject (*create_raw_callback)(void*, std::shared_ptr<void>,
                                 void (*)(void*, JNIEnv*, jstring)) = nullptr;
  void (*clear_raw_callback)(void*, jobject) = nullptr;
  jobject (*create_async_handler)(void*, std::shared_ptr<void>,
                                  void (*)(void*, JNIEnv*, jstring,
                                           jstring)) = nullptr;
  void (*clear_async_handler)(void*, jobject) = nullptr;

  bool complete() const {
    return message_bus && context && create_raw_callback &&
           clear_raw_callback && create_async_handler && clear_async_handler;
  }
};

// Installs both legacy protocol subscriptions and correlated async handlers
// before native bootstrap. MICROPHONE_ACCESS follows host capture
// configuration; LOCAL_NETWORK needs no extra runtime permission, matching the
// Android APK. No query/request opens an audio device, enables a disabled
// microphone, or changes Roblox account eligibility. Other capabilities remain
// unsupported.
class RobloxPermissionsBridge final {
 public:
  RobloxPermissionsBridge(JniEnvironmentProvider environment,
                          RobloxPermissionsMessageBusSymbols symbols,
                          RobloxPermissionsMessageBusObjects objects,
                          bool microphone_enabled);
  ~RobloxPermissionsBridge();
  RobloxPermissionsBridge(const RobloxPermissionsBridge&) = delete;
  RobloxPermissionsBridge& operator=(const RobloxPermissionsBridge&) = delete;

  Status Initialize();
  Status Shutdown();

 private:
  struct State;
  Status ShutdownLocked();

  const JniEnvironmentProvider environment_;
  const RobloxPermissionsMessageBusSymbols symbols_;
  const RobloxPermissionsMessageBusObjects objects_;
  const bool microphone_enabled_;
  // Serializes lifecycle only. Native dispatch never holds this mutex.
  std::mutex lifecycle_mutex_;
  std::shared_ptr<State> state_;
};

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_ROBLOX_PERMISSIONS_BRIDGE_H_
