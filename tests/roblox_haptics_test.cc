#include "runtime/roblox_haptics_bridge.h"

#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>
#include <vector>

#include "jnivm/jnivm.h"

namespace mocktail {
namespace runtime {
namespace {

using Json = nlohmann::json;

std::string Copy(JNIEnv* env, jstring value) {
  if (!value)
    return {};
  const char* chars = env->GetStringUTFChars(value, nullptr);
  const std::string copy = chars ? chars : "";
  if (chars)
    env->ReleaseStringUTFChars(value, chars);
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
  std::mutex mutex;
  struct Published {
    std::string method;
    Json body;
    jint code;
  };
  std::vector<Published> published;
  std::vector<std::pair<std::string, Json>> resolved;
  std::vector<std::pair<bool, Json>> capabilities;
};

Probe* g_probe = nullptr;

void Prepare(void* context) {
  static_cast<jnivm::VM*>(context)->RestoreFunctions();
}

jobject CreateRaw(void* context, std::shared_ptr<void> target,
                  void (*run)(void*, JNIEnv*, jstring)) {
  auto* probe = static_cast<Probe*>(context);
  ++probe->callbacks_created;
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
  if (message)
    env->DeleteLocalRef(message);
  env->DeleteLocalRef(id);
  env->DeleteLocalRef(cls);
}

jobject Subscribe(JNIEnv* env, jobject, jstring protocol, jstring method,
                  jobject callback, jboolean once) {
  EXPECT_EQ(Copy(env, protocol), "HapticProtocol");
  EXPECT_EQ(once, JNI_FALSE);
  g_probe->legacy[Copy(env, method)] = callback;
  const int handle = ++g_probe->subscriptions;
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
  EXPECT_EQ(Copy(env, protocol), "HapticProtocol");
  g_probe->handlers[Copy(env, method)] = handler;
}

void ClearHandler(JNIEnv* env, jobject, jstring protocol, jstring) {
  EXPECT_EQ(Copy(env, protocol), "HapticProtocol");
  ++g_probe->native_cleared;
}

void Publish(JNIEnv* env, jobject, jstring protocol, jstring method,
             jstring response, jint code, jstring telemetry) {
  EXPECT_EQ(Copy(env, protocol), "HapticProtocol");
  EXPECT_EQ(Copy(env, telemetry), "{}");
  std::lock_guard<std::mutex> lock(g_probe->mutex);
  g_probe->published.push_back(
      {Copy(env, method), Json::parse(Copy(env, response)), code});
}

void Resolve(JNIEnv* env, jobject, jstring id, jstring body) {
  std::lock_guard<std::mutex> lock(g_probe->mutex);
  g_probe->resolved.emplace_back(Copy(env, id), Json::parse(Copy(env, body)));
}

void UpdateState(JNIEnv* env, jclass, jboolean enabled, jstring motors) {
  std::lock_guard<std::mutex> lock(g_probe->mutex);
  g_probe->capabilities.emplace_back(enabled == JNI_TRUE,
                                     Json::parse(Copy(env, motors)));
}

constexpr char kMotorState[] =
    R"({"defaultMotor":{"intensities":[0.4],"millisecondsPerSample":10}})";

class RobloxHapticsBridgeTest : public testing::Test {
 protected:
  void SetUp() override {
    probe.vm = &vm;
    g_probe = &probe;
    env = vm.GetJNIEnv();
    jclass cls =
        env->FindClass("com/roblox/universalapp/messagebus/MessageBus");
    bus = env->AllocObject(cls);
    env->DeleteLocalRef(cls);
    vm.RegisterClass("com/roblox/engine/jni/NativeGLInterface");
    native_gl =
        env->FindClass("com/roblox/engine/jni/NativeGLInterface");
    ASSERT_NE(native_gl, nullptr);
  }

  void TearDown() override {
    bridge.reset();
    env->DeleteLocalRef(native_gl);
    env->DeleteLocalRef(bus);
    g_probe = nullptr;
  }

  Status Initialize(bool available = true) {
    haptics = std::make_shared<RobloxHaptics>();
    haptics->SetAvailable(available);
    bridge = std::make_unique<RobloxHapticsBridge>(
        JniEnvironmentProvider{vm.GetJavaVM(), &vm, Prepare},
        RobloxMessageBusSymbols{Subscribe, Disconnect, Publish, SetHandler,
                                ClearHandler, Resolve},
        RobloxMessageBusObjects{bus, &probe, CreateRaw, ClearRaw, CreateAsync,
                                ClearAsync},
        haptics, &UpdateState);
    return bridge->Initialize();
  }

  jnivm::VM vm;
  Probe probe;
  JNIEnv* env = nullptr;
  jobject bus = nullptr;
  jclass native_gl = nullptr;
  std::shared_ptr<RobloxHaptics> haptics;
  std::unique_ptr<RobloxHapticsBridge> bridge;
};

TEST(RobloxHapticsTest, SingletonVibrationSamplesConstantIntensity) {
  RobloxHaptics haptics;
  haptics.SetAvailable(true);
  ASSERT_TRUE(
      haptics.Submit("UpdateSingletonVibration", R"({"intensity":0.5})"));
  EXPECT_FLOAT_EQ(haptics.Sample(1000000), 0.5f);
  EXPECT_FLOAT_EQ(haptics.Sample(2000000000), 0.5f);
  haptics.Cancel();
  EXPECT_FLOAT_EQ(haptics.Sample(3000000000), 0.0f);
}

TEST(RobloxHapticsTest, MotorStateSamplesOnTimeline) {
  RobloxHaptics haptics;
  haptics.SetAvailable(true);
  ASSERT_TRUE(haptics.Submit("SetMotorState", kMotorState));
  EXPECT_FLOAT_EQ(haptics.Sample(100000000), 0.4f);
  ASSERT_TRUE(haptics.Submit(
      "SetMotorState",
      R"({"defaultMotor":{"intensities":[0.2,0.8],"millisecondsPerSample":10}})"));
  EXPECT_FLOAT_EQ(haptics.Sample(200000000), 0.2f);
  EXPECT_FLOAT_EQ(haptics.Sample(215000000), 0.8f);
  EXPECT_FLOAT_EQ(haptics.Sample(225000000), 0.0f);
}

TEST(RobloxHapticsTest, EmptyMotorStateStopsPlayback) {
  RobloxHaptics haptics;
  haptics.SetAvailable(true);
  ASSERT_TRUE(haptics.Submit("SetMotorState", kMotorState));
  EXPECT_FLOAT_EQ(haptics.Sample(1), 0.4f);
  ASSERT_TRUE(haptics.Submit("SetMotorState", "{}"));
  EXPECT_FLOAT_EQ(haptics.Sample(2), 0.0f);
}

TEST(RobloxHapticsTest, RejectsMalformedRequests) {
  RobloxHaptics haptics;
  haptics.SetAvailable(true);
  EXPECT_FALSE(haptics.Submit("SetMotorState", "{"));
  EXPECT_FALSE(haptics.Submit("UpdateSingletonVibration", "{}"));
  EXPECT_FALSE(haptics.Submit("UpdateSingletonVibration",
                              R"({"intensity":"fast"})"));
  EXPECT_FALSE(haptics.Submit("SetMotorState",
                              R"({"other":{"intensities":[1]}})"));
  EXPECT_FALSE(haptics.Submit("Rumble", R"({"intensity":1})"));
  EXPECT_TRUE(haptics.Submit("UpdateSingletonVibration",
                             R"({"intensity":"0.3"})"));
  EXPECT_FLOAT_EQ(haptics.Sample(1), 0.3f);
  ASSERT_TRUE(
      haptics.Submit("UpdateSingletonVibration", R"({"intensity":4})"));
  EXPECT_FLOAT_EQ(haptics.Sample(2), 1.0f);
}

TEST(RobloxHapticsTest, GatesSubmissionsOnState) {
  RobloxHaptics haptics;
  EXPECT_TRUE(haptics.Submit("UpdateSingletonVibration",
                             R"({"intensity":1})"));
  EXPECT_FLOAT_EQ(haptics.Sample(1), 0.0f);
  haptics.SetAvailable(true);
  EXPECT_TRUE(haptics.Submit("UpdateSingletonVibration",
                             R"({"intensity":1})"));
  EXPECT_FLOAT_EQ(haptics.Sample(2), 1.0f);
  haptics.SetFocused(false);
  EXPECT_FLOAT_EQ(haptics.Sample(3), 0.0f);
  haptics.SetFocused(true);
  EXPECT_FLOAT_EQ(haptics.Sample(4), 0.0f);
  haptics.SetEnabled(false);
  EXPECT_TRUE(haptics.Submit("UpdateSingletonVibration",
                             R"({"intensity":1})"));
  EXPECT_FLOAT_EQ(haptics.Sample(5), 0.0f);
  EXPECT_FALSE(haptics.available());
}

TEST_F(RobloxHapticsBridgeTest, RegistersAllMethodsOnBothTransports) {
  ASSERT_TRUE(Initialize().ok());
  EXPECT_EQ(probe.legacy.size(), 4u);
  EXPECT_EQ(probe.handlers.size(), 4u);
  for (const char* method : {"SupportsHaptics", "UpdateSingletonVibration",
                             "GetMotors", "SetMotorState"}) {
    EXPECT_TRUE(probe.legacy.count(method)) << method;
    EXPECT_TRUE(probe.handlers.count(method)) << method;
  }
  EXPECT_FALSE(bridge->Initialize().ok());
  ASSERT_TRUE(bridge->Shutdown().ok());
  EXPECT_EQ(probe.disconnected, 4);
  EXPECT_EQ(probe.native_cleared, 4);
  EXPECT_EQ(probe.callbacks_cleared, 4);
  EXPECT_EQ(probe.handlers_cleared, 4);
}

TEST_F(RobloxHapticsBridgeTest, SupportsHapticsReflectsRumbleAvailability) {
  ASSERT_TRUE(Initialize().ok());
  InvokeLegacy(env, probe.legacy.at("SupportsHaptics"), "{}");
  ASSERT_FALSE(probe.published.empty());
  EXPECT_EQ(probe.published.back().body["supportsHaptics"], true);
  haptics->SetAvailable(false);
  InvokeAsync(env, probe.handlers.at("SupportsHaptics"), "{}", "cap-1");
  ASSERT_FALSE(probe.resolved.empty());
  EXPECT_EQ(probe.resolved.back().second["supportsHaptics"], false);
}

TEST_F(RobloxHapticsBridgeTest, GetMotorsReturnsDefaultMotorWhenAvailable) {
  ASSERT_TRUE(Initialize().ok());
  InvokeLegacy(env, probe.legacy.at("GetMotors"), "{}");
  EXPECT_EQ(probe.published.back().body["defaultMotor"]["x"], 0);
  haptics->SetAvailable(false);
  InvokeAsync(env, probe.handlers.at("GetMotors"), "{}", "motors-1");
  EXPECT_TRUE(probe.resolved.back().second.empty());
}

TEST_F(RobloxHapticsBridgeTest, MotorUpdatesSkipLegacyReplies) {
  ASSERT_TRUE(Initialize().ok());
  InvokeLegacy(env, probe.legacy.at("UpdateSingletonVibration"),
               R"({"intensity":0.6})");
  InvokeLegacy(env, probe.legacy.at("SetMotorState"), kMotorState);
  EXPECT_TRUE(probe.published.empty());
  EXPECT_FLOAT_EQ(haptics->Sample(1), 0.4f);
  InvokeAsync(env, probe.handlers.at("UpdateSingletonVibration"),
              R"({"intensity":0.9})", "vib-1");
  ASSERT_EQ(probe.resolved.size(), 1u);
  EXPECT_EQ(probe.resolved.back().first, "vib-1");
  EXPECT_EQ(probe.published.back().code, 0);
  EXPECT_FLOAT_EQ(haptics->Sample(2), 0.9f);
}

TEST_F(RobloxHapticsBridgeTest, RejectedMotorUpdatesReportErrorCode) {
  ASSERT_TRUE(Initialize().ok());
  InvokeAsync(env, probe.handlers.at("SetMotorState"), "{", "bad-1");
  ASSERT_FALSE(probe.resolved.empty());
  EXPECT_EQ(probe.published.back().code, 13);
}

TEST_F(RobloxHapticsBridgeTest,
       RefreshCapabilitiesNotifiesNativeOnlyOnChange) {
  ASSERT_TRUE(Initialize(false).ok());
  bridge->RefreshCapabilities(native_gl);
  ASSERT_EQ(probe.capabilities.size(), 1u);
  EXPECT_FALSE(probe.capabilities[0].first);
  EXPECT_TRUE(probe.capabilities[0].second.empty());
  bridge->RefreshCapabilities(native_gl);
  EXPECT_EQ(probe.capabilities.size(), 1u);
  haptics->SetAvailable(true);
  bridge->RefreshCapabilities(native_gl);
  ASSERT_EQ(probe.capabilities.size(), 2u);
  EXPECT_TRUE(probe.capabilities[1].first);
  EXPECT_EQ(probe.capabilities[1].second["defaultMotor"]["z"], 0);
  bridge->RefreshCapabilities(nullptr);
  EXPECT_EQ(probe.capabilities.size(), 2u);
}

TEST_F(RobloxHapticsBridgeTest, ShutdownDisablesStaleCallbacks) {
  ASSERT_TRUE(Initialize().ok());
  jobject handler =
      env->NewGlobalRef(probe.handlers.at("SetMotorState"));
  ASSERT_TRUE(bridge->Shutdown().ok());
  InvokeAsync(env, handler, kMotorState, "stale");
  EXPECT_TRUE(probe.published.empty());
  EXPECT_TRUE(probe.resolved.empty());
  env->DeleteGlobalRef(handler);
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
