#include "jnivm/jnivm.h"

#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace jnivm {
namespace {

TEST(JniVmMessageBusTest, ReturnsOnlyTheExactMessageBusSingletonContract) {
  VM vm;
  JNIEnv *env = vm.GetJNIEnv();
  jclass bus_class =
      env->FindClass("com/roblox/universalapp/messagebus/MessageBus");
  ASSERT_NE(bus_class, nullptr);
  jmethodID exact = env->GetStaticMethodID(
      bus_class, "f", "()Lcom/roblox/universalapp/messagebus/MessageBus;");
  ASSERT_NE(exact, nullptr);
  jobject first = env->CallStaticObjectMethod(bus_class, exact);
  jobject second = env->CallStaticObjectMethod(bus_class, exact);
  EXPECT_NE(first, nullptr);
  EXPECT_EQ(first, second);

  jclass wrong_class = env->FindClass("java/lang/Object");
  EXPECT_EQ(env->CallStaticObjectMethod(wrong_class, exact), nullptr);
  jmethodID wrong_signature =
      env->GetStaticMethodID(bus_class, "f", "()Ljava/lang/Object;");
  EXPECT_EQ(env->CallStaticObjectMethod(bus_class, wrong_signature), nullptr);
}

struct RawCallbackProbe {
  std::mutex mutex;
  std::condition_variable entered;
  std::condition_variable released;
  std::string message;
  bool is_entered = false;
  bool may_return = false;
};

void RecordRawMessage(void *context, JNIEnv *env, jstring message) {
  auto *probe = static_cast<RawCallbackProbe *>(context);
  const char *chars = env->GetStringUTFChars(message, nullptr);
  {
    std::unique_lock<std::mutex> lock(probe->mutex);
    probe->message = chars != nullptr ? chars : "";
    probe->is_entered = true;
    probe->entered.notify_all();
    probe->released.wait(lock, [probe] { return probe->may_return; });
  }
  if (chars != nullptr) {
    env->ReleaseStringUTFChars(message, chars);
  }
}

TEST(JniVmMessageBusTest, PreservesNativeConnectionConstructorHandle) {
  VM vm;
  JNIEnv *env = vm.GetJNIEnv();
  ASSERT_NE(env, nullptr);
  jclass connection_class =
      env->FindClass("com/roblox/universalapp/messagebus/Connection");
  ASSERT_NE(connection_class, nullptr);
  jmethodID constructor = env->GetMethodID(connection_class, "<init>", "(J)V");
  ASSERT_NE(constructor, nullptr);

  constexpr jlong kNativeHandle = 0x123456789LL;
  jobject connection =
      env->NewObject(connection_class, constructor, kNativeHandle);
  ASSERT_NE(connection, nullptr);
  jfieldID field = env->GetFieldID(connection_class, "f10205a", "J");
  ASSERT_NE(field, nullptr);
  EXPECT_EQ(env->GetLongField(connection, field), kNativeHandle);
  jfieldID alias = env->GetFieldID(connection_class, "nativePtr", "J");
  ASSERT_NE(alias, nullptr);
  EXPECT_EQ(env->GetLongField(connection, alias), kNativeHandle);
}

TEST(JniVmMessageBusTest, ClearStopsNewCallsAndRetainsInflightContext) {
  VM vm;
  auto probe = std::make_shared<RawCallbackProbe>();
  std::weak_ptr<RawCallbackProbe> weak_probe = probe;
  jobject callback = vm.CreateMessageBusRawCallback(
      probe, MessageBusRawCallbacks{&RecordRawMessage});
  ASSERT_NE(callback, nullptr);
  JNIEnv *env = vm.GetJNIEnv();
  jclass callback_class = env->GetObjectClass(callback);
  jmethodID run =
      env->GetMethodID(callback_class, "run", "(Ljava/lang/String;)V");
  ASSERT_NE(run, nullptr);
  jstring message = env->NewStringUTF("{\"placeId\":1}");

  std::thread worker([env, callback, run, message] {
    env->CallVoidMethod(callback, run, message);
  });
  {
    std::unique_lock<std::mutex> lock(probe->mutex);
    ASSERT_TRUE(probe->entered.wait_for(lock, std::chrono::seconds(2),
                                        [probe] { return probe->is_entered; }));
  }
  vm.ClearMessageBusRawCallback(callback);
  probe.reset();
  EXPECT_FALSE(weak_probe.expired());
  {
    std::shared_ptr<RawCallbackProbe> retained = weak_probe.lock();
    ASSERT_NE(retained, nullptr);
    std::lock_guard<std::mutex> lock(retained->mutex);
    retained->may_return = true;
    retained->released.notify_all();
  }
  worker.join();
  EXPECT_TRUE(weak_probe.expired());

  env->CallVoidMethod(callback, run, message);
  EXPECT_FALSE(vm.DispatchMessageBusRawCallback(callback, env, message));
}

} // namespace
} // namespace jnivm
