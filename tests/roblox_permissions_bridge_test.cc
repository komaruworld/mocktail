#include "runtime/roblox_permissions_bridge.h"

#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <thread>
#include <vector>

#include "jnivm/jnivm.h"

namespace mocktail {
namespace runtime {
namespace {

using Json = nlohmann::json;
constexpr char kRequest[] = R"({"permissions":["MICROPHONE_ACCESS"]})";

std::string Copy(JNIEnv* env, jstring value) {
  if (!value) return {};
  const char* chars = env->GetStringUTFChars(value, nullptr);
  const std::string copy = chars ? chars : "";
  if (chars) env->ReleaseStringUTFChars(value, chars);
  return copy;
}

struct Probe {
  jnivm::VM* vm = nullptr;
  std::map<std::string, jobject> legacy;
  std::map<std::string, jobject> handlers;
  int callbacks_created = 0;
  int callbacks_cleared = 0;
  int handlers_created = 0;
  int handlers_cleared = 0;
  int subscriptions = 0;
  int disconnected = 0;
  int native_cleared = 0;
  int fail_callback_at = 0;
  int fail_subscription_at = 0;
  bool call_during_registration = false;
  bool reenter = false;
  std::mutex mutex;
  std::condition_variable changed;
  bool block_publish = false;
  bool publish_entered = false;
  bool release_publish = false;
  struct Published {
    std::string method;
    Json body;
    jint code;
  };
  std::vector<Published> published;
  std::vector<std::pair<std::string, Json>> resolved;
};

Probe* g_probe = nullptr;

void Prepare(void* context) {
  static_cast<jnivm::VM*>(context)->RestoreFunctions();
}

jobject CreateRaw(void* context, std::shared_ptr<void> target,
                  void (*run)(void*, JNIEnv*, jstring)) {
  auto* probe = static_cast<Probe*>(context);
  if (++probe->callbacks_created == probe->fail_callback_at) return nullptr;
  return probe->vm->CreateMessageBusRawCallback(
      std::move(target), jnivm::MessageBusRawCallbacks{run});
}

void ClearRaw(void* context, jobject callback) {
  auto* probe = static_cast<Probe*>(context);
  ++probe->callbacks_cleared;
  probe->vm->ClearMessageBusRawCallback(callback);
}

jobject CreateAsync(void* context, std::shared_ptr<void> target,
                    void (*run)(void*, JNIEnv*, jstring, jstring)) {
  auto* probe = static_cast<Probe*>(context);
  ++probe->handlers_created;
  return probe->vm->CreateMessageBusAsyncRequestHandler(
      std::move(target), jnivm::MessageBusAsyncRequestHandlerCallbacks{run});
}

void ClearAsync(void* context, jobject handler) {
  auto* probe = static_cast<Probe*>(context);
  ++probe->handlers_cleared;
  probe->vm->ClearMessageBusAsyncRequestHandler(handler);
}

void InvokeLegacy(JNIEnv* env, jobject callback, const char* payload) {
  jclass cls = env->GetObjectClass(callback);
  jmethodID run = env->GetMethodID(cls, "run", "(Ljava/lang/String;)V");
  jstring message = env->NewStringUTF(payload);
  env->CallVoidMethod(callback, run, message);
  env->DeleteLocalRef(message);
  env->DeleteLocalRef(cls);
}

void InvokeAsync(JNIEnv* env, jobject handler, const char* payload,
                 const char* response_id) {
  jclass cls = env->GetObjectClass(handler);
  jmethodID run =
      env->GetMethodID(cls, "run", "(Ljava/lang/String;Ljava/lang/String;)V");
  jstring message = payload ? env->NewStringUTF(payload) : nullptr;
  jstring id = env->NewStringUTF(response_id);
  jvalue args[2]{};
  args[0].l = message;
  args[1].l = id;
  env->CallVoidMethodA(handler, run, args);
  if (message) env->DeleteLocalRef(message);
  env->DeleteLocalRef(id);
  env->DeleteLocalRef(cls);
}

jobject Subscribe(JNIEnv* env, jobject, jstring protocol, jstring method,
                  jobject callback, jboolean once) {
  EXPECT_EQ(Copy(env, protocol), "PermissionsProtocol");
  EXPECT_EQ(once, JNI_FALSE);
  const int handle = ++g_probe->subscriptions;
  if (handle == g_probe->fail_subscription_at) return nullptr;
  g_probe->legacy[Copy(env, method)] = callback;
  if (g_probe->call_during_registration) InvokeLegacy(env, callback, kRequest);
  jclass cls = env->FindClass("com/roblox/universalapp/messagebus/Connection");
  jmethodID ctor = env->GetMethodID(cls, "<init>", "(J)V");
  jobject connection = env->NewObject(cls, ctor, static_cast<jlong>(handle));
  env->DeleteLocalRef(cls);
  return connection;
}

void Disconnect(JNIEnv*, jobject, jlong handle) {
  EXPECT_GT(handle, 0);
  ++g_probe->disconnected;
}

void SetHandler(JNIEnv* env, jobject, jstring protocol, jstring method,
                jobject handler) {
  EXPECT_EQ(Copy(env, protocol), "PermissionsProtocol");
  g_probe->handlers[Copy(env, method)] = handler;
}

void ClearHandler(JNIEnv* env, jobject, jstring protocol, jstring) {
  EXPECT_EQ(Copy(env, protocol), "PermissionsProtocol");
  ++g_probe->native_cleared;
}

void Publish(JNIEnv* env, jobject, jstring protocol, jstring method,
             jstring response, jint code, jstring telemetry) {
  EXPECT_EQ(Copy(env, protocol), "PermissionsProtocol");
  EXPECT_EQ(Copy(env, telemetry), "{}");
  {
    std::unique_lock<std::mutex> lock(g_probe->mutex);
    g_probe->published.push_back(
        {Copy(env, method), Json::parse(Copy(env, response)), code});
    if (g_probe->block_publish) {
      g_probe->publish_entered = true;
      g_probe->changed.notify_all();
      g_probe->changed.wait(lock, [] { return g_probe->release_publish; });
    }
  }
  if (g_probe->reenter) {
    g_probe->reenter = false;
    InvokeLegacy(env, g_probe->legacy.at("HasPermissions"), kRequest);
  }
}

void Resolve(JNIEnv* env, jobject, jstring id, jstring body) {
  std::lock_guard<std::mutex> lock(g_probe->mutex);
  g_probe->resolved.emplace_back(Copy(env, id), Json::parse(Copy(env, body)));
}

class RobloxPermissionsBridgeTest : public testing::Test {
 protected:
  void SetUp() override {
    probe.vm = &vm;
    g_probe = &probe;
    env = vm.GetJNIEnv();
    jclass cls =
        env->FindClass("com/roblox/universalapp/messagebus/MessageBus");
    bus = env->AllocObject(cls);
    env->DeleteLocalRef(cls);
  }
  void TearDown() override {
    bridge.reset();
    env->DeleteLocalRef(bus);
    g_probe = nullptr;
  }
  Status Initialize(bool enabled = true) {
    bridge = std::make_unique<RobloxPermissionsBridge>(
        JniEnvironmentProvider{vm.GetJavaVM(), &vm, Prepare},
        RobloxPermissionsMessageBusSymbols{Subscribe, Disconnect, Publish,
                                           SetHandler, ClearHandler, Resolve},
        RobloxPermissionsMessageBusObjects{bus, &probe, CreateRaw, ClearRaw,
                                           CreateAsync, ClearAsync},
        enabled);
    return bridge->Initialize();
  }
  Probe::Published Legacy(const char* method, const char* payload = kRequest) {
    InvokeLegacy(env, probe.legacy.at(method), payload);
    return probe.published.back();
  }
  Json Async(const char* method, const char* payload = kRequest,
             const char* id = "request-1") {
    InvokeAsync(env, probe.handlers.at(method), payload, id);
    return probe.resolved.back().second;
  }

  jnivm::VM vm;
  Probe probe;
  JNIEnv* env = nullptr;
  jobject bus = nullptr;
  std::unique_ptr<RobloxPermissionsBridge> bridge;
};

TEST_F(RobloxPermissionsBridgeTest, RegistersBothTransportsAndReinitializes) {
  ASSERT_TRUE(Initialize().ok());
  EXPECT_EQ(probe.legacy.size(), 5u);
  EXPECT_EQ(probe.handlers.size(), 5u);
  EXPECT_FALSE(bridge->Initialize().ok());
  ASSERT_TRUE(bridge->Shutdown().ok());
  EXPECT_EQ(probe.disconnected, 5);
  EXPECT_EQ(probe.native_cleared, 5);
  EXPECT_EQ(probe.callbacks_cleared, 5);
  EXPECT_EQ(probe.handlers_cleared, 5);
  ASSERT_TRUE(bridge->Shutdown().ok());
  EXPECT_EQ(probe.disconnected, 5);
  ASSERT_TRUE(bridge->Initialize().ok());
  EXPECT_EQ(probe.subscriptions, 10);
  EXPECT_EQ(Async("HasPermissions").at("status"), "AUTHORIZED");
}

TEST_F(RobloxPermissionsBridgeTest,
       SupportsMicrophoneAndNetworkWithoutGrantingOthers) {
  ASSERT_TRUE(Initialize().ok());
  const Json expected = {
      {"permissions", {"MICROPHONE_ACCESS", "LOCAL_NETWORK"}}};
  EXPECT_EQ(Legacy("SupportsPermissions", "{}").body, expected);
  EXPECT_EQ(Async("SupportsPermissions", "{}"), expected);
  for (const char* method : {"HasPermissions", "PermissionsRequest"}) {
    const Json authorized = {{"status", "AUTHORIZED"},
                             {"missingPermissions", Json::array()}};
    EXPECT_EQ(Legacy(method).body, authorized);
    EXPECT_EQ(Async(method), authorized);
    const char* mixed =
        R"({"permissions":["MICROPHONE_ACCESS","LOCAL_NETWORK","CAMERA_ACCESS","CONTACTS_ACCESS","unknown"]})";
    const Json denied = {{"status", "DENIED"},
                         {"missingPermissions",
                          {"CAMERA_ACCESS", "CONTACTS_ACCESS", "unknown"}}};
    EXPECT_EQ(Legacy(method, mixed).body, denied);
    EXPECT_EQ(Async(method, mixed), denied);
  }
}

TEST_F(RobloxPermissionsBridgeTest,
       DisabledMicrophoneCannotBeGrantedByRequest) {
  ASSERT_TRUE(Initialize(false).ok());
  EXPECT_EQ(Async("SupportsPermissions", "{}").at("permissions"),
            Json::array({"MICROPHONE_ACCESS", "LOCAL_NETWORK"}));
  for (const char* method : {"HasPermissions", "PermissionsRequest"}) {
    const Json denied = {{"status", "DENIED"},
                         {"missingPermissions", {"MICROPHONE_ACCESS"}}};
    EXPECT_EQ(Legacy(method).body, denied);
    EXPECT_EQ(Async(method), denied);
  }
  EXPECT_EQ(Async("HasPermissions").at("status"), "DENIED");
  EXPECT_EQ(Async("HasPermissions", R"({"permissions":["LOCAL_NETWORK"]})")
                .at("status"),
            "AUTHORIZED");
  EXPECT_EQ(Async("PermissionsRequest",
                  R"({"permissions":["MICROPHONE_ACCESS","LOCAL_NETWORK"]})")
                .at("missingPermissions"),
            Json::array({"MICROPHONE_ACCESS"}));
}

TEST_F(RobloxPermissionsBridgeTest, HidesAndroidOnlyUpsellAndRationale) {
  ASSERT_TRUE(Initialize(false).ok());
  const Json hidden = {{"upsellStatus", "HIDE"},
                       {"hiddenUpsellPermissions", {"MICROPHONE_ACCESS"}}};
  for (const char* method :
       {"ShouldShowPermissionUpsell", "ShouldShowRequestPermissionRationale"}) {
    EXPECT_EQ(Legacy(method).body, hidden);
    EXPECT_EQ(Async(method), hidden);
  }
}

TEST_F(RobloxPermissionsBridgeTest, MalformedRequestsFailClosedAndResolve) {
  ASSERT_TRUE(Initialize().ok());
  const std::vector<std::string> invalid = {
      "",
      "not json",
      "[]",
      "null",
      "{}",
      R"({"permissions":"MICROPHONE_ACCESS"})",
      R"({"permissions":[null]})",
      R"({"permissions":[42]})",
      R"({"permissions":[""]})",
      R"({"permissions":["MICROPHONE_ACCESS\u0000"]})",
      R"({"permissions":["MICROPHONE_ACCESS\n"]})",
      "{\"permissions\":[] ,\"extra\":" + std::string(20, '[') + "0" +
          std::string(20, ']') + "}",
      Json{{"permissions", {std::string(129, 'x')}}}.dump(),
      Json{{"permissions", std::vector<std::string>(65, "MICROPHONE_ACCESS")}}
          .dump(),
      std::string(64 * 1024 + 1, ' ')};
  for (const std::string& value : invalid) {
    SCOPED_TRACE(value.substr(0, 150));
    const auto response = Legacy("PermissionsRequest", value.c_str());
    EXPECT_EQ(response.code, 13);
    EXPECT_EQ(response.body.at("status"), "DENIED");
    EXPECT_EQ(Async("HasPermissions", value.c_str()).at("status"), "DENIED");
    EXPECT_EQ(probe.published.back().code, 13);
  }
  EXPECT_EQ(Async("HasPermissions", nullptr).at("status"), "DENIED");
  const auto supports = Legacy("SupportsPermissions", "[]");
  EXPECT_EQ(supports.code, 13);
  EXPECT_TRUE(supports.body.at("permissions").empty());
}

TEST_F(RobloxPermissionsBridgeTest, AsyncResponsesPreserveCorrelationIds) {
  ASSERT_TRUE(Initialize().ok());
  EXPECT_EQ(Async("HasPermissions", kRequest, "first").at("status"),
            "AUTHORIZED");
  EXPECT_EQ(
      Async("HasPermissions", R"({"permissions":["CAMERA_ACCESS"]})", "second")
          .at("status"),
      "DENIED");
  ASSERT_EQ(probe.resolved.size(), 2u);
  EXPECT_EQ(probe.resolved[0].first, "first");
  EXPECT_EQ(probe.resolved[1].first, "second");
  ASSERT_EQ(probe.published.size(), 2u);
  EXPECT_EQ(probe.resolved[0].second, probe.published[0].body);
  EXPECT_EQ(probe.resolved[1].second, probe.published[1].body);
}

TEST_F(RobloxPermissionsBridgeTest, RollbackClearsPartiallyCreatedCallbacks) {
  probe.fail_callback_at = 3;
  EXPECT_FALSE(Initialize().ok());
  EXPECT_EQ(probe.disconnected, 2);
  EXPECT_EQ(probe.native_cleared, 2);
  EXPECT_EQ(probe.callbacks_cleared, 2);
  EXPECT_EQ(probe.handlers_cleared, 3);
  EXPECT_TRUE(bridge->Shutdown().ok());
  probe.fail_callback_at = 0;
  EXPECT_TRUE(bridge->Initialize().ok());
}

TEST_F(RobloxPermissionsBridgeTest, RollbackAfterNativeSubscriptionFailure) {
  probe.fail_subscription_at = 3;
  EXPECT_FALSE(Initialize().ok());
  EXPECT_EQ(probe.disconnected, 2);
  EXPECT_EQ(probe.native_cleared, 2);
  EXPECT_EQ(probe.callbacks_cleared, 3);
  EXPECT_EQ(probe.handlers_cleared, 3);
}

TEST_F(RobloxPermissionsBridgeTest,
       RespondsDuringRegistrationAndReentrantCalls) {
  probe.call_during_registration = true;
  ASSERT_TRUE(Initialize().ok());
  ASSERT_EQ(probe.published.size(), 5u);
  probe.reenter = true;
  Async("HasPermissions");
  EXPECT_EQ(probe.published.size(), 7u);
}

TEST_F(RobloxPermissionsBridgeTest, StaleCallbacksAreInertAfterShutdown) {
  ASSERT_TRUE(Initialize().ok());
  jobject legacy = env->NewGlobalRef(probe.legacy.at("HasPermissions"));
  jobject handler = env->NewGlobalRef(probe.handlers.at("HasPermissions"));
  ASSERT_TRUE(bridge->Shutdown().ok());
  ASSERT_TRUE(bridge->Initialize().ok());
  InvokeLegacy(env, legacy, kRequest);
  InvokeAsync(env, handler, kRequest, "stale");
  EXPECT_TRUE(probe.published.empty());
  EXPECT_TRUE(probe.resolved.empty());
  EXPECT_EQ(Async("HasPermissions").at("status"), "AUTHORIZED");
  env->DeleteGlobalRef(legacy);
  env->DeleteGlobalRef(handler);
}

TEST_F(RobloxPermissionsBridgeTest,
       ShutdownDrainsInflightResponseBeforeCleanup) {
  ASSERT_TRUE(Initialize().ok());
  probe.block_publish = true;
  std::thread caller([&] {
    InvokeAsync(env, probe.handlers.at("HasPermissions"), kRequest,
                "in-flight");
  });
  bool entered;
  {
    std::unique_lock<std::mutex> lock(probe.mutex);
    entered = probe.changed.wait_for(lock, std::chrono::seconds(2),
                                     [&] { return probe.publish_entered; });
  }
  EXPECT_TRUE(entered);
  auto shutdown =
      std::async(std::launch::async, [&] { return bridge->Shutdown(); });
  EXPECT_EQ(shutdown.wait_for(std::chrono::milliseconds(30)),
            std::future_status::timeout);
  {
    std::lock_guard<std::mutex> lock(probe.mutex);
    probe.release_publish = true;
    probe.changed.notify_all();
  }
  caller.join();
  EXPECT_TRUE(shutdown.get().ok());
  ASSERT_EQ(probe.resolved.size(), 1u);
  EXPECT_EQ(probe.resolved.front().first, "in-flight");
  EXPECT_EQ(probe.disconnected, 5);
}

TEST_F(RobloxPermissionsBridgeTest, RejectsIncompleteNativeContract) {
  RobloxPermissionsBridge incomplete(
      {vm.GetJavaVM(), &vm, Prepare}, {},
      {bus, &probe, CreateRaw, ClearRaw, CreateAsync, ClearAsync}, true);
  EXPECT_FALSE(incomplete.Initialize().ok());
  EXPECT_EQ(probe.callbacks_created, 0);
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
