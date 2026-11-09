#ifndef MOCKTAIL_RUNTIME_ROBLOX_MESSAGE_BUS_REQUEST_BRIDGE_H_
#define MOCKTAIL_RUNTIME_ROBLOX_MESSAGE_BUS_REQUEST_BRIDGE_H_

#include <jni.h>

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "mocktail/status.h"
#include "runtime/roblox_game_session_native_adapter.h"

namespace mocktail {
namespace runtime {
struct RobloxMessageBusSymbols {
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

struct RobloxMessageBusObjects {
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

struct RobloxMessageBusReply {
  std::string json;
  jint code = 0;
  const char* outcome = "ok";
};

// Owns both legacy subscriptions and correlated async request handlers.
// A policy receives only a request's method index and JNI-local payload.
class RobloxMessageBusRequestBridge final {
 public:
  using Handler =
      std::function<RobloxMessageBusReply(JNIEnv*, jstring, std::size_t)>;
  RobloxMessageBusRequestBridge(JniEnvironmentProvider environment,
                                RobloxMessageBusSymbols symbols,
                                RobloxMessageBusObjects objects,
                                std::string protocol,
                                std::vector<std::string> methods,
                                Handler handler);
  ~RobloxMessageBusRequestBridge();
  Status Initialize();
  Status Shutdown();

 private:
  struct State;
  Status ShutdownLocked();
  const JniEnvironmentProvider environment_;
  const RobloxMessageBusSymbols symbols_;
  const RobloxMessageBusObjects objects_;
  const std::string protocol_name_;
  const std::vector<std::string> methods_;
  const Handler handler_;
  std::mutex lifecycle_mutex_;
  std::shared_ptr<State> state_;
};
}  // namespace runtime
}  // namespace mocktail
#endif
