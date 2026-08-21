#ifndef MOCKTAIL_RUNTIME_ROBLOX_EXPERIENCE_LAUNCH_BRIDGE_H_
#define MOCKTAIL_RUNTIME_ROBLOX_EXPERIENCE_LAUNCH_BRIDGE_H_

#include <jni.h>

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

#include "mocktail/status.h"
#include "runtime/roblox_game_session_native_adapter.h"

namespace mocktail {
namespace runtime {

inline constexpr std::size_t kMaximumExperienceLaunchJsonBytes = 64 * 1024;

// Owned equivalent of the APK ug.l launch contract. Strings are copied before
// the MessageBus RawCallback returns, so no JNI local reference escapes.
struct RobloxExperienceLaunchRequest {
  int64_t place_id = 0;
  int64_t user_id = 0;
  int64_t conversation_id = 0;
  int64_t referred_by_player_id = 0;
  std::string game_instance_id;
  std::string reserved_server_access_code;
  std::string call_id;
  std::string referral_page;
  std::string access_code;
  std::string link_code;
  std::string launch_data;
  std::string event_id;
  std::string game_join_context;
  std::string join_attempt_id;
  std::string join_attempt_origin;
  std::string iso_context;
  std::string canonical_json;
};

Status ParseRobloxExperienceLaunchJson(const std::string &json,
                                       RobloxExperienceLaunchRequest *request);

GameJoinRequest
MakeGameJoinRequest(uint64_t request_id,
                    const RobloxExperienceLaunchRequest &request);

using DispatchExperienceLaunchFn =
    Status (*)(void *context, const RobloxExperienceLaunchRequest &request);

struct RobloxExperienceLaunchSink {
  void *context = nullptr;
  DispatchExperienceLaunchFn dispatch = nullptr;

  bool valid() const { return dispatch != nullptr; }
};

using GetExperienceLaunchIdFn = jstring (*)(JNIEnv *, jclass);
using SubscribeExperienceLaunchRawFn = jobject (*)(JNIEnv *, jobject, jstring,
                                                   jobject, jboolean);
using DeleteMessageBusConnectionFn = void (*)(JNIEnv *, jobject, jlong);

struct RobloxExperienceMessageBusSymbols {
  GetExperienceLaunchIdFn get_launch_id = nullptr;
  SubscribeExperienceLaunchRawFn subscribe_raw = nullptr;
  DeleteMessageBusConnectionFn delete_connection = nullptr;

  bool complete() const {
    return get_launch_id != nullptr && subscribe_raw != nullptr &&
           delete_connection != nullptr;
  }
};

struct RobloxExperienceMessageBusObjects {
  jobject message_bus = nullptr;
  void *callback_factory_context = nullptr;
  jobject (*create_raw_callback)(void *factory_context,
                                 std::shared_ptr<void> callback_context,
                                 void (*run)(void *, JNIEnv *,
                                             jstring)) = nullptr;
  void (*clear_raw_callback)(void *factory_context, jobject callback) = nullptr;

  bool complete() const {
    return message_bus != nullptr && callback_factory_context != nullptr &&
           create_raw_callback != nullptr && clear_raw_callback != nullptr;
  }
};

// API-first adapter for JNIExperienceProtocol + MessageBus.doSubscribeRaw.
//
// The pseudo-JVM must route RawCallback.run(String) to HandleRawMessage. It
// must also preserve Connection.<init>(J)'s f10205a field; Initialize rejects a
// connection whose native shared_ptr handle cannot be recovered, because such
// a subscription cannot be released safely.
class RobloxExperienceLaunchBridge final {
public:
  RobloxExperienceLaunchBridge(JniEnvironmentProvider environment,
                               RobloxExperienceMessageBusSymbols symbols,
                               RobloxExperienceMessageBusObjects objects,
                               RobloxExperienceLaunchSink sink);
  ~RobloxExperienceLaunchBridge();

  RobloxExperienceLaunchBridge(const RobloxExperienceLaunchBridge &) = delete;
  RobloxExperienceLaunchBridge &
  operator=(const RobloxExperienceLaunchBridge &) = delete;

  Status Initialize();
  Status Shutdown();

  // Entry point for the pseudo-JVM RawCallback.run(String) shim.
  Status HandleRawMessage(JNIEnv *env, jstring message);

  // Owned entry point used by tests and by alternative upstream MessageBus
  // adapters. It follows the same bounded parser and dispatch path.
  Status HandleOwnedMessage(std::string message);

  bool initialized() const;

private:
  Status CheckJniException(JNIEnv *env, const char *operation) const;
  Status BeginDispatch();
  void EndDispatch();
  static void RawMessageCallback(void *context, JNIEnv *env, jstring message);

  struct RawCallbackTarget;

  const JniEnvironmentProvider environment_;
  const RobloxExperienceMessageBusSymbols symbols_;
  const RobloxExperienceMessageBusObjects source_objects_;
  const RobloxExperienceLaunchSink sink_;

  mutable std::mutex mutex_;
  std::condition_variable dispatch_drained_;
  jobject message_bus_ = nullptr;
  jobject raw_callback_ = nullptr;
  jobject connection_ = nullptr;
  jlong connection_handle_ = 0;
  std::shared_ptr<RawCallbackTarget> raw_callback_target_;
  std::size_t in_flight_dispatches_ = 0;
  bool initializing_ = false;
  bool shutting_down_ = false;
  bool initialized_ = false;
  bool accepting_messages_ = false;
};

} // namespace runtime
} // namespace mocktail

#endif // MOCKTAIL_RUNTIME_ROBLOX_EXPERIENCE_LAUNCH_BRIDGE_H_
