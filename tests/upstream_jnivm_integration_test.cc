#include <dlfcn.h>

#include <atomic>
#include <thread>

#include <gtest/gtest.h>
#include <jnivm.h>

namespace {

using JniOnLoad = jint (*)(JavaVM*, void*);
using NativeIncrement = jint (*)(JNIEnv*, jclass, jint);

TEST(UpstreamJniVmIntegrationTest, SharedLibraryRegistersNativeMethod) {
  void* fixture =
      ::dlopen(MOCKTAIL_UPSTREAM_JNI_FIXTURE_PATH, RTLD_NOW | RTLD_LOCAL);
  const char* load_error = fixture == nullptr ? ::dlerror() : nullptr;
  ASSERT_NE(fixture, nullptr)
      << (load_error != nullptr ? load_error : "unknown error");

  auto* jni_on_load =
      reinterpret_cast<JniOnLoad>(::dlsym(fixture, "JNI_OnLoad"));
  ASSERT_NE(jni_on_load, nullptr);

  jnivm::VM vm;
  EXPECT_EQ(jni_on_load(vm.GetJavaVM(), nullptr), JNI_VERSION_1_6);

  auto fixture_class =
      vm.GetEnv()->GetClass("com/mocktail/UpstreamFixture");
  ASSERT_NE(fixture_class, nullptr);
  auto native = fixture_class->natives.find("nativeIncrement");
  ASSERT_NE(native, fixture_class->natives.end());

  JNIEnv* env = vm.GetJNIEnv();
  jclass java_class = env->FindClass("com/mocktail/UpstreamFixture");
  ASSERT_NE(java_class, nullptr);
  auto* increment = reinterpret_cast<NativeIncrement>(native->second);
  EXPECT_EQ(increment(env, java_class, 41), 42);

  EXPECT_EQ(::dlclose(fixture), 0);
}

TEST(UpstreamJniVmIntegrationTest, TypedHookWorksOnAttachedThread) {
  jnivm::VM vm;
  auto host_hooks = vm.GetEnv()->GetClass("com/mocktail/HostHooks");
  host_hooks->Hook(vm.GetEnv().get(), "twice",
                   [](jint value) -> jint { return value * 2; });

  JNIEnv* main_env = vm.GetJNIEnv();
  jclass main_class = main_env->FindClass("com/mocktail/HostHooks");
  jmethodID main_method =
      main_env->GetStaticMethodID(main_class, "twice", "(I)I");
  ASSERT_NE(main_method, nullptr);
  EXPECT_EQ(main_env->CallStaticIntMethod(main_class, main_method, 21), 42);

  std::atomic<jint> attach_result{JNI_ERR};
  std::atomic<jint> get_env_result{JNI_ERR};
  std::atomic<jint> detach_result{JNI_ERR};
  std::atomic<jint> hook_result{-1};
  std::atomic<bool> received_distinct_env{false};
  std::atomic<bool> get_env_matched{false};

  std::thread worker([&] {
    JNIEnv* attached_env = nullptr;
    attach_result =
        vm.GetJavaVM()->AttachCurrentThread(&attached_env, nullptr);
    if (attach_result != JNI_OK || attached_env == nullptr) {
      return;
    }

    received_distinct_env = attached_env != main_env;
    void* queried_env = nullptr;
    get_env_result =
        vm.GetJavaVM()->GetEnv(&queried_env, JNI_VERSION_1_6);
    get_env_matched = queried_env == attached_env;

    jclass worker_class = attached_env->FindClass("com/mocktail/HostHooks");
    jmethodID worker_method =
        attached_env->GetStaticMethodID(worker_class, "twice", "(I)I");
    if (worker_class != nullptr && worker_method != nullptr) {
      hook_result =
          attached_env->CallStaticIntMethod(worker_class, worker_method, 11);
    }
    detach_result = vm.GetJavaVM()->DetachCurrentThread();
  });
  worker.join();

  EXPECT_EQ(attach_result, JNI_OK);
  EXPECT_EQ(get_env_result, JNI_OK);
  EXPECT_EQ(detach_result, JNI_OK);
  EXPECT_TRUE(received_distinct_env);
  EXPECT_TRUE(get_env_matched);
  EXPECT_EQ(hook_result, 22);
}

}  // namespace
