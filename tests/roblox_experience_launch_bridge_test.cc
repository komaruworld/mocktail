#include "runtime/roblox_experience_launch_bridge.h"

#include "jnivm/jnivm.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

namespace mocktail {
namespace runtime {
namespace {

struct BridgeProbe {
  jobject subscribed_callback = nullptr;
  std::shared_ptr<void> raw_callback_context;
  void (*run_raw_callback)(void *, JNIEnv *, jstring) = nullptr;
  jlong deleted_handle = 0;
  int dispatches = 0;
  bool dispatch_during_subscribe = false;
  RobloxExperienceLaunchRequest request;
};

BridgeProbe *g_bridge_probe = nullptr;

struct ShutdownRaceProbe {
  std::mutex mutex;
  std::condition_variable changed;
  bool string_copy_entered = false;
  bool allow_string_copy = false;
  bool connection_deleted = false;
};

ShutdownRaceProbe *g_shutdown_race_probe = nullptr;
jsize (*g_original_get_string_utf_length)(JNIEnv *, jstring) = nullptr;

jstring GetLaunchId(JNIEnv *env, jclass) {
  return env->NewStringUTF("experienceLaunch");
}

jobject SubscribeRaw(JNIEnv *env, jobject, jstring, jobject callback,
                     jboolean) {
  g_bridge_probe->subscribed_callback = callback;
  if (g_bridge_probe->dispatch_during_subscribe) {
    jclass callback_class = env->GetObjectClass(callback);
    jmethodID run =
        env->GetMethodID(callback_class, "run", "(Ljava/lang/String;)V");
    jstring json = env->NewStringUTF(
        R"({"placeId":17580461965,"gameJoinContext":"PlayButton"})");
    env->CallVoidMethod(callback, run, json);
    env->DeleteLocalRef(json);
    env->DeleteLocalRef(callback_class);
  }
  jclass connection =
      env->FindClass("com/roblox/universalapp/messagebus/Connection");
  jmethodID constructor = env->GetMethodID(connection, "<init>", "(J)V");
  return env->NewObject(connection, constructor, static_cast<jlong>(77));
}

void DeleteConnection(JNIEnv *, jobject, jlong handle) {
  g_bridge_probe->deleted_handle = handle;
}

void DeleteConnectionDuringShutdownRace(JNIEnv *, jobject, jlong handle) {
  g_bridge_probe->deleted_handle = handle;
  std::lock_guard<std::mutex> lock(g_shutdown_race_probe->mutex);
  g_shutdown_race_probe->connection_deleted = true;
  g_shutdown_race_probe->changed.notify_all();
}

jsize BlockedGetStringUtfLength(JNIEnv *env, jstring value) {
  {
    std::unique_lock<std::mutex> lock(g_shutdown_race_probe->mutex);
    g_shutdown_race_probe->string_copy_entered = true;
    g_shutdown_race_probe->changed.notify_all();
    g_shutdown_race_probe->changed.wait(
        lock, [] { return g_shutdown_race_probe->allow_string_copy; });
  }
  return g_original_get_string_utf_length(env, value);
}

jobject CreateRawCallback(void *context, std::shared_ptr<void> callback_context,
                          void (*run)(void *, JNIEnv *, jstring)) {
  auto *vm = static_cast<jnivm::VM *>(context);
  g_bridge_probe->raw_callback_context = callback_context;
  g_bridge_probe->run_raw_callback = run;
  return vm->CreateMessageBusRawCallback(std::move(callback_context),
                                         jnivm::MessageBusRawCallbacks{run});
}

void ClearRawCallback(void *context, jobject callback) {
  static_cast<jnivm::VM *>(context)->ClearMessageBusRawCallback(callback);
}

Status DispatchLaunch(void *context,
                      const RobloxExperienceLaunchRequest &request) {
  auto *probe = static_cast<BridgeProbe *>(context);
  ++probe->dispatches;
  probe->request = request;
  return Status::Ok();
}

TEST(RobloxExperienceLaunchParserTest, ParsesApkLaunchContract) {
  RobloxExperienceLaunchRequest request;
  const Status status = ParseRobloxExperienceLaunchJson(
      R"({"placeId":17580461965,"userId":42,"conversationId":7,)"
      R"("gameInstanceId":"instance","reservedServerAccessCode":"reserved",)"
      R"("callId":"call","referralPage":"gameDetail","accessCode":"access",)"
      R"("linkCode":"link","launchData":"data","referredByPlayerId":9,)"
      R"("eventId":"event","gameJoinContext":"PlayButton",)"
      R"("joinAttemptId":"attempt","joinAttemptOrigin":"LuaApp",)"
      R"("isoContext":"iso"})",
      &request);

  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(request.place_id, 17580461965);
  EXPECT_EQ(request.user_id, 42);
  EXPECT_EQ(request.conversation_id, 7);
  EXPECT_EQ(request.referred_by_player_id, 9);
  EXPECT_EQ(request.game_instance_id, "instance");
  EXPECT_EQ(request.reserved_server_access_code, "reserved");
  EXPECT_EQ(request.game_join_context, "PlayButton");
  EXPECT_EQ(request.join_attempt_origin, "LuaApp");
  EXPECT_FALSE(request.canonical_json.empty());
  const GameJoinRequest join = MakeGameJoinRequest(12, request);
  EXPECT_EQ(join.request_id, 12);
  EXPECT_EQ(join.place_id, 17580461965);
  EXPECT_EQ(join.launch_parameters, request.canonical_json);
}

TEST(RobloxExperienceLaunchParserTest, RejectsMissingOrInvalidPlace) {
  RobloxExperienceLaunchRequest request;
  EXPECT_FALSE(ParseRobloxExperienceLaunchJson("{}", &request).ok());
  EXPECT_FALSE(
      ParseRobloxExperienceLaunchJson(R"({"placeId":0})", &request).ok());
  EXPECT_FALSE(
      ParseRobloxExperienceLaunchJson(R"({"placeId":"17580461965"})", &request)
          .ok());
  EXPECT_FALSE(ParseRobloxExperienceLaunchJson(
                   R"({"placeId":17580461965,"userId":-1})", &request)
                   .ok());
  EXPECT_FALSE(ParseRobloxExperienceLaunchJson(
                   R"({"placeId":18446744073709551615})", &request)
                   .ok());
}

TEST(RobloxExperienceLaunchParserTest, RejectsMalformedAndUnboundedPayloads) {
  RobloxExperienceLaunchRequest request;
  EXPECT_FALSE(ParseRobloxExperienceLaunchJson("[1,2,3]", &request).ok());
  EXPECT_FALSE(ParseRobloxExperienceLaunchJson("{", &request).ok());
  EXPECT_FALSE(
      ParseRobloxExperienceLaunchJson(
          std::string(kMaximumExperienceLaunchJsonBytes + 1, 'x'), &request)
          .ok());
  EXPECT_FALSE(ParseRobloxExperienceLaunchJson(
                   std::string("{\"placeId\":1,\"launchData\":\"") +
                       std::string(16 * 1024 + 1, 'x') + "\"}",
                   &request)
                   .ok());
}

TEST(RobloxExperienceLaunchParserTest, AllowsUnknownForwardCompatibleFields) {
  RobloxExperienceLaunchRequest request;
  const Status status = ParseRobloxExperienceLaunchJson(
      R"({"placeId":1,"futureField":{"nested":true}})", &request);
  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(request.place_id, 1);
}

TEST(RobloxExperienceLaunchBridgeTest,
     SubscribesDispatchesAndDeletesExactNativeConnection) {
  jnivm::VM vm;
  JNIEnv *env = vm.GetJNIEnv();
  jclass bus_class =
      env->FindClass("com/roblox/universalapp/messagebus/MessageBus");
  jobject bus = env->AllocObject(bus_class);
  BridgeProbe probe;
  g_bridge_probe = &probe;
  RobloxExperienceLaunchBridge bridge(
      {vm.GetJavaVM(), nullptr, nullptr},
      {&GetLaunchId, &SubscribeRaw, &DeleteConnection},
      {bus, &vm, &CreateRawCallback, &ClearRawCallback},
      {&probe, &DispatchLaunch});

  ASSERT_TRUE(bridge.Initialize().ok());
  ASSERT_NE(probe.subscribed_callback, nullptr);
  jclass callback_class = env->GetObjectClass(probe.subscribed_callback);
  jmethodID run =
      env->GetMethodID(callback_class, "run", "(Ljava/lang/String;)V");
  jstring json = env->NewStringUTF(
      R"({"placeId":17580461965,"gameJoinContext":"PlayButton"})");
  env->CallVoidMethod(probe.subscribed_callback, run, json);
  ASSERT_EQ(probe.dispatches, 1);
  EXPECT_EQ(probe.request.place_id, 17580461965);
  EXPECT_EQ(probe.request.game_join_context, "PlayButton");

  ASSERT_TRUE(bridge.Shutdown().ok());
  EXPECT_EQ(probe.deleted_handle, 77);
  env->CallVoidMethod(probe.subscribed_callback, run, json);
  EXPECT_EQ(probe.dispatches, 1);
  g_bridge_probe = nullptr;
}

TEST(RobloxExperienceLaunchBridgeTest,
     AcceptsRawCallbackInvokedSynchronouslyBySubscribe) {
  jnivm::VM vm;
  JNIEnv *env = vm.GetJNIEnv();
  jclass bus_class =
      env->FindClass("com/roblox/universalapp/messagebus/MessageBus");
  jobject bus = env->AllocObject(bus_class);
  BridgeProbe probe;
  probe.dispatch_during_subscribe = true;
  g_bridge_probe = &probe;
  RobloxExperienceLaunchBridge bridge(
      {vm.GetJavaVM(), nullptr, nullptr},
      {&GetLaunchId, &SubscribeRaw, &DeleteConnection},
      {bus, &vm, &CreateRawCallback, &ClearRawCallback},
      {&probe, &DispatchLaunch});

  const Status initialize = bridge.Initialize();
  ASSERT_TRUE(initialize.ok()) << initialize.message();
  EXPECT_TRUE(bridge.initialized());
  ASSERT_EQ(probe.dispatches, 1);
  EXPECT_EQ(probe.request.place_id, 17580461965);
  EXPECT_EQ(probe.request.game_join_context, "PlayButton");

  ASSERT_TRUE(bridge.Shutdown().ok());
  EXPECT_EQ(probe.deleted_handle, 77);
  g_bridge_probe = nullptr;
}

TEST(RobloxExperienceLaunchBridgeTest,
     ShutdownDoesNotInvertRawCallbackAndBridgeLocks) {
  jnivm::VM vm;
  JNIEnv *env = vm.GetJNIEnv();
  jclass bus_class =
      env->FindClass("com/roblox/universalapp/messagebus/MessageBus");
  jobject bus = env->AllocObject(bus_class);
  BridgeProbe probe;
  ShutdownRaceProbe race;
  g_bridge_probe = &probe;
  g_shutdown_race_probe = &race;
  RobloxExperienceLaunchBridge bridge(
      {vm.GetJavaVM(), nullptr, nullptr},
      {&GetLaunchId, &SubscribeRaw, &DeleteConnectionDuringShutdownRace},
      {bus, &vm, &CreateRawCallback, &ClearRawCallback},
      {&probe, &DispatchLaunch});

  ASSERT_TRUE(bridge.Initialize().ok());
  ASSERT_NE(probe.subscribed_callback, nullptr);
  ASSERT_NE(probe.raw_callback_context, nullptr);
  ASSERT_NE(probe.run_raw_callback, nullptr);
  jstring json = env->NewStringUTF(
      R"({"placeId":17580461965,"gameJoinContext":"PlayButton"})");

  JNINativeInterface_ blocking_functions = *env->functions;
  g_original_get_string_utf_length = blocking_functions.GetStringUTFLength;
  blocking_functions.GetStringUTFLength = &BlockedGetStringUtfLength;
  JNIEnv blocking_env = {};
  blocking_env.functions = &blocking_functions;

  std::atomic<bool> callback_completed = false;
  std::thread callback_thread([&] {
    probe.run_raw_callback(probe.raw_callback_context.get(), &blocking_env,
                           json);
    callback_completed.store(true, std::memory_order_release);
  });

  bool callback_holds_target = false;
  {
    std::unique_lock<std::mutex> lock(race.mutex);
    callback_holds_target =
        race.changed.wait_for(lock, std::chrono::seconds(2),
                              [&] { return race.string_copy_entered; });
  }

  Status shutdown_status;
  std::thread shutdown_thread([&] { shutdown_status = bridge.Shutdown(); });
  bool disconnect_started = false;
  {
    std::unique_lock<std::mutex> lock(race.mutex);
    disconnect_started = race.changed.wait_for(
        lock, std::chrono::seconds(2), [&] { return race.connection_deleted; });
  }

  // Keep RawMessageCallback paused while shutdown crosses the disconnect and
  // starts invalidating its callback target. The old bridge-mutex -> target
  // order deadlocked here when the callback resumed and entered BeginDispatch.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  EXPECT_FALSE(callback_completed.load(std::memory_order_acquire));
  {
    std::lock_guard<std::mutex> lock(race.mutex);
    race.allow_string_copy = true;
    race.changed.notify_all();
  }

  callback_thread.join();
  shutdown_thread.join();

  EXPECT_TRUE(callback_holds_target);
  EXPECT_TRUE(disconnect_started);
  EXPECT_TRUE(shutdown_status.ok()) << shutdown_status.message();
  EXPECT_TRUE(callback_completed.load(std::memory_order_acquire));
  EXPECT_EQ(probe.dispatches, 0);
  EXPECT_EQ(probe.deleted_handle, 77);
  g_original_get_string_utf_length = nullptr;
  g_shutdown_race_probe = nullptr;
  g_bridge_probe = nullptr;
}

} // namespace
} // namespace runtime
} // namespace mocktail
