#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "jnivm/jnivm.h"

namespace jnivm {
namespace {

class JniFieldsTest : public testing::Test {
 protected:
  void SetUp() override {
    env = vm.GetJNIEnv();
    clazz = env->FindClass("test/Fields");
  }

  std::string ReadString(jobject value) {
    const auto string = static_cast<jstring>(value);
    const char* bytes = env->GetStringUTFChars(string, nullptr);
    const std::string result = bytes ? bytes : "";
    if (bytes) env->ReleaseStringUTFChars(string, bytes);
    return result;
  }

  VM vm;
  JNIEnv* env = nullptr;
  jclass clazz = nullptr;
};

TEST_F(JniFieldsTest, StaticFieldOwnsItsValueAfterLocalReferenceIsDeleted) {
  const auto field =
      env->GetStaticFieldID(clazz, "saved", "Ljava/lang/String;");
  const auto value = env->NewStringUTF("retained");
  env->SetStaticObjectField(clazz, field, value);
  env->DeleteLocalRef(value);
  const auto other = env->NewStringUTF("replacement");
  const auto saved = env->GetStaticObjectField(clazz, field);
  EXPECT_EQ(ReadString(saved), "retained");
  env->DeleteLocalRef(saved);
  env->DeleteLocalRef(other);
  env->SetStaticObjectField(clazz, field, nullptr);
}

TEST_F(JniFieldsTest, ReadingStaticFieldReturnsAnIndependentLocalReference) {
  const auto field = env->GetStaticFieldID(clazz, "read", "Ljava/lang/String;");
  const auto value = env->NewStringUTF("retained");
  env->SetStaticObjectField(clazz, field, value);
  const auto first = env->GetStaticObjectField(clazz, field);
  env->DeleteLocalRef(first);
  const auto second = env->GetStaticObjectField(clazz, field);
  EXPECT_EQ(ReadString(second), "retained");
  env->SetStaticObjectField(clazz, field, nullptr);
  EXPECT_EQ(ReadString(second), "retained");
  env->DeleteLocalRef(second);
  env->DeleteLocalRef(value);
}

TEST_F(JniFieldsTest, FieldIdsOwnTemporaryNames) {
  auto object = env->AllocObject(clazz);
  char name[] = "original";
  const auto field = env->GetFieldID(clazz, name, "I");
  const auto static_field =
      env->GetStaticFieldID(clazz, name, "Ljava/lang/String;");
  std::memcpy(name, "modified", sizeof(name));
  env->SetIntField(object, field, 42);
  EXPECT_EQ(env->GetIntField(object, env->GetFieldID(clazz, "original", "I")),
            42);
  const auto value = env->NewStringUTF("stable");
  env->SetStaticObjectField(clazz, static_field, value);
  const auto saved = env->GetStaticObjectField(
      clazz, env->GetStaticFieldID(clazz, "original", "Ljava/lang/String;"));
  EXPECT_EQ(ReadString(saved), "stable");
  env->DeleteLocalRef(saved);
  env->DeleteLocalRef(value);
  env->SetStaticObjectField(clazz, static_field, nullptr);
  env->DeleteLocalRef(object);
}

TEST_F(JniFieldsTest, ReclaimsObjectFieldReferencesAcrossManyAllocations) {
  const auto field = env->GetFieldID(clazz, "child", "Ljava/lang/String;");
  for (int iteration = 0; iteration < 110000; ++iteration) {
    const auto parent = env->AllocObject(clazz);
    const auto child = env->NewStringUTF("child");
    ASSERT_NE(parent, nullptr) << iteration;
    ASSERT_NE(child, nullptr) << iteration;
    env->SetObjectField(parent, field, child);
    env->DeleteLocalRef(child);
    env->DeleteLocalRef(parent);
  }
}

TEST_F(JniFieldsTest, ReleasingParentPreservesAnExplicitGlobalChildReference) {
  const auto field = env->GetFieldID(clazz, "child", "Ljava/lang/String;");
  const auto parent = env->AllocObject(clazz);
  const auto child = env->NewStringUTF("survives");
  const auto global = env->NewGlobalRef(child);
  env->SetObjectField(parent, field, child);
  env->DeleteLocalRef(child);
  env->DeleteLocalRef(parent);
  EXPECT_EQ(ReadString(global), "survives");
  env->DeleteGlobalRef(global);
}

TEST_F(JniFieldsTest, InstanceGetterReturnsAnIndependentLocalReference) {
  const auto field = env->GetFieldID(clazz, "child", "Ljava/lang/String;");
  const auto parent = env->AllocObject(clazz);
  const auto child = env->NewStringUTF("survives");
  env->SetObjectField(parent, field, child);
  env->DeleteLocalRef(child);
  const auto result = env->CallObjectMethod(
      parent, env->GetMethodID(clazz, "getChild", "()Ljava/lang/String;"));
  env->DeleteLocalRef(parent);
  EXPECT_EQ(ReadString(result), "survives");
  env->DeleteLocalRef(result);
}

TEST_F(JniFieldsTest, StaticCallbackSetterOwnsItsImplementation) {
  const auto interface =
      env->FindClass("com/roblox/engine/jni/NativeGLJavaInterface");
  const auto callback_class =
      env->FindClass("com/roblox/engine/jni/EngineJavaCallback2");
  const auto callback = env->AllocObject(callback_class);
  const auto marker = env->GetFieldID(callback_class, "marker", "I");
  env->SetIntField(callback, marker, 73);
  const auto setter =
      env->GetStaticMethodID(interface, "setImplementation",
                             "(Lcom/roblox/engine/jni/EngineJavaCallback2;)V");
  env->CallStaticVoidMethod(interface, setter, callback);
  env->DeleteLocalRef(callback);
  const auto other = env->AllocObject(callback_class);
  env->SetIntField(other, marker, 99);
  const auto getter =
      env->GetStaticMethodID(interface, "getImplementation",
                             "()Lcom/roblox/engine/jni/EngineJavaCallback2;");
  const auto saved = env->CallStaticObjectMethod(interface, getter);
  EXPECT_EQ(env->GetIntField(saved, marker), 73);
  env->DeleteLocalRef(saved);
  env->DeleteLocalRef(other);
  env->CallStaticVoidMethod(interface, setter, static_cast<jobject>(nullptr));
}

TEST_F(JniFieldsTest, ReclaimsObjectArrayReferencesAcrossManyAllocations) {
  for (int iteration = 0; iteration < 110000; ++iteration) {
    const auto child = env->NewStringUTF("child");
    ASSERT_NE(child, nullptr) << iteration;
    const auto array = env->NewObjectArray(2, clazz, child);
    ASSERT_NE(array, nullptr);
    env->DeleteLocalRef(child);
    const auto element = env->GetObjectArrayElement(array, 1);
    EXPECT_EQ(ReadString(element), "child");
    env->DeleteLocalRef(element);
    env->DeleteLocalRef(array);
  }
}

TEST_F(JniFieldsTest, PopLocalFramePromotesOnlyOneResultReference) {
  for (int iteration = 0; iteration < 110000; ++iteration) {
    ASSERT_EQ(env->PushLocalFrame(3), JNI_OK);
    const auto value = env->NewStringUTF("result");
    ASSERT_NE(value, nullptr) << iteration;
    env->NewLocalRef(value);
    env->NewLocalRef(value);
    const auto result = env->PopLocalFrame(value);
    ASSERT_EQ(ReadString(result), "result");
    env->DeleteLocalRef(result);
  }
}

TEST_F(JniFieldsTest, ConcurrentPrimitiveFieldWritesPreserveAllValues) {
  constexpr int kThreads = 4;
  constexpr int kFields = 1000;
  std::vector<std::array<jfieldID, 3>> ids;
  for (int i = 0; i < kThreads * kFields; ++i) {
    const std::string name = std::to_string(i);
    ids.push_back({env->GetFieldID(clazz, (name + "i").c_str(), "I"),
                   env->GetFieldID(clazz, (name + "b").c_str(), "Z"),
                   env->GetFieldID(clazz, (name + "f").c_str(), "F")});
  }
  const auto local = env->AllocObject(clazz);
  const auto object = env->NewGlobalRef(local);
  env->DeleteLocalRef(local);
  std::atomic<int> ready{0};
  std::array<std::thread, kThreads> workers;
  for (int thread = 0; thread < kThreads; ++thread) {
    workers[thread] = std::thread([&, thread] {
      JNIEnv* worker = nullptr;
      const jint attached = vm.GetJavaVM()->AttachCurrentThread(
          reinterpret_cast<void**>(&worker), nullptr);
      ready.fetch_add(1);
      while (ready.load() != kThreads) std::this_thread::yield();
      ASSERT_EQ(attached, JNI_OK);
      for (int i = thread * kFields; i < (thread + 1) * kFields; ++i) {
        worker->SetIntField(object, ids[i][0], i);
        worker->SetBooleanField(object, ids[i][1], JNI_TRUE);
        worker->SetFloatField(object, ids[i][2], static_cast<float>(i));
      }
      EXPECT_EQ(vm.GetJavaVM()->DetachCurrentThread(), JNI_OK);
    });
  }
  for (auto& worker : workers) worker.join();
  for (int i = 0; i < kThreads * kFields; ++i) {
    EXPECT_EQ(env->GetIntField(object, ids[i][0]), i);
    EXPECT_EQ(env->GetBooleanField(object, ids[i][1]), JNI_TRUE);
    EXPECT_EQ(env->GetFloatField(object, ids[i][2]), static_cast<float>(i));
  }
  env->DeleteGlobalRef(object);
}

}  // namespace
}  // namespace jnivm
