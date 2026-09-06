#include <gtest/gtest.h>

#include <atomic>
#include <condition_variable>
#include <cstdarg>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "jnivm/jnivm.h"

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

struct AsyncProbe {
  std::string message;
  std::string id;
  int calls = 0;
};

void RecordAsync(void* context, JNIEnv* env, jstring message, jstring id) {
  auto* probe = static_cast<AsyncProbe*>(context);
  ++probe->calls;
  const char* text =
      message ? env->GetStringUTFChars(message, nullptr) : nullptr;
  probe->message = text ? text : "";
  if (text) env->ReleaseStringUTFChars(message, text);
  text = env->GetStringUTFChars(id, nullptr);
  probe->id = text ? text : "";
  if (text) env->ReleaseStringUTFChars(id, text);
}

void InvokeV(JNIEnv* env, jobject receiver, jmethodID method, ...) {
  va_list args;
  va_start(args, method);
  env->CallVoidMethodV(receiver, method, args);
  va_end(args);
}

class JniVmAsyncRequestTest : public testing::TestWithParam<int> {
 protected:
  void Invoke(JNIEnv* env, jobject receiver, jmethodID method, jstring message,
              jstring id) {
    if (GetParam() == 0) {
      env->functions->CallVoidMethod(env, receiver, method, message, id);
    } else if (GetParam() == 1) {
      InvokeV(env, receiver, method, message, id);
    } else {
      jvalue args[2]{};
      args[0].l = message;
      args[1].l = id;
      env->CallVoidMethodA(receiver, method, args);
    }
  }
};

TEST_P(JniVmAsyncRequestTest, ExactTwoStringContractAndClear) {
  VM vm;
  auto probe = std::make_shared<AsyncProbe>();
  jobject handler = vm.CreateMessageBusAsyncRequestHandler(
      probe, MessageBusAsyncRequestHandlerCallbacks{RecordAsync});
  ASSERT_NE(handler, nullptr);
  JNIEnv* env = vm.GetJNIEnv();
  jclass cls = env->GetObjectClass(handler);
  jmethodID run =
      env->GetMethodID(cls, "run", "(Ljava/lang/String;Ljava/lang/String;)V");
  jstring message = env->NewStringUTF("{\"permissions\":[]}");
  jstring id = env->NewStringUTF("request-42");
  Invoke(env, handler, run, message, id);
  EXPECT_EQ(probe->calls, 1);
  EXPECT_EQ(probe->message, "{\"permissions\":[]}");
  EXPECT_EQ(probe->id, "request-42");
  Invoke(env, handler, run, nullptr, id);
  EXPECT_EQ(probe->calls, 2);  // Malformed input still gets a denial response.
  Invoke(env, handler, run, message, nullptr);
  EXPECT_EQ(probe->calls, 2);
  vm.ClearMessageBusAsyncRequestHandler(handler);
  Invoke(env, handler, run, message, id);
  EXPECT_EQ(probe->calls, 2);
}

TEST_P(JniVmAsyncRequestTest,
       RejectsWrongSignaturesReceiversAndUnboundObjects) {
  VM vm;
  auto probe = std::make_shared<AsyncProbe>();
  jobject handler = vm.CreateMessageBusAsyncRequestHandler(
      probe, MessageBusAsyncRequestHandlerCallbacks{RecordAsync});
  JNIEnv* env = vm.GetJNIEnv();
  jclass cls = env->GetObjectClass(handler);
  jstring value = env->NewStringUTF("{}");
  for (const char* signature : {"()V", "(Ljava/lang/String;)V",
                                "(Ljava/lang/String;Ljava/lang/String;)Z"}) {
    Invoke(env, handler, env->GetMethodID(cls, "run", signature), value, value);
  }
  jmethodID run =
      env->GetMethodID(cls, "run", "(Ljava/lang/String;Ljava/lang/String;)V");
  Invoke(env, env->AllocObject(cls), run, value, value);
  Invoke(env, env->AllocObject(env->FindClass("java/lang/Object")), run, value,
         value);
  Invoke(
      env, handler,
      env->GetMethodID(cls, "other", "(Ljava/lang/String;Ljava/lang/String;)V"),
      value, value);
  EXPECT_EQ(probe->calls, 0);
}

INSTANTIATE_TEST_SUITE_P(DirectVAndA, JniVmAsyncRequestTest,
                         testing::Values(0, 1, 2));

void BlockAsync(void* context, JNIEnv* env, jstring message, jstring) {
  RecordRawMessage(context, env, message);
}

TEST(JniVmMessageBusTest, AsyncClearRetainsInflightContext) {
  VM vm;
  auto probe = std::make_shared<RawCallbackProbe>();
  std::weak_ptr<RawCallbackProbe> weak = probe;
  jobject handler = vm.CreateMessageBusAsyncRequestHandler(
      probe, MessageBusAsyncRequestHandlerCallbacks{BlockAsync});
  ASSERT_NE(handler, nullptr);
  JNIEnv* env = vm.GetJNIEnv();
  jstring message = env->NewStringUTF("{}");
  jstring id = env->NewStringUTF("id");
  std::thread caller([&] {
    vm.DispatchMessageBusAsyncRequestHandler(handler, env, message, id);
  });
  {
    std::unique_lock<std::mutex> lock(probe->mutex);
    EXPECT_TRUE(probe->entered.wait_for(lock, std::chrono::seconds(2),
                                        [&] { return probe->is_entered; }));
  }
  vm.ClearMessageBusAsyncRequestHandler(handler);
  probe.reset();
  EXPECT_FALSE(weak.expired());
  if (const auto held = weak.lock()) {
    std::lock_guard<std::mutex> lock(held->mutex);
    held->may_return = true;
    held->released.notify_all();
  }
  caller.join();
  EXPECT_TRUE(weak.expired());
  EXPECT_FALSE(
      vm.DispatchMessageBusAsyncRequestHandler(handler, env, message, id));
}

} // namespace
} // namespace jnivm
