#include "jnivm/jnivm.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace jnivm {
namespace {

void CallVoidMethodVForTest(JNIEnv* env, jobject receiver,
                            jmethodID method_id, ...) {
  va_list args;
  va_start(args, method_id);
  env->functions->CallVoidMethodV(env, receiver, method_id, args);
  va_end(args);
}

void CallStaticVoidMethodVForTest(JNIEnv* env, jclass receiver,
                                  jmethodID method_id, ...) {
  va_list args;
  va_start(args, method_id);
  env->functions->CallStaticVoidMethodV(env, receiver, method_id, args);
  va_end(args);
}

void IncrementReturnCount(void* context) {
  auto* count = static_cast<std::atomic<int>*>(context);
  ++(*count);
}

TEST(JniVmExperienceLifecycleTest,
     RoutesExactLegacyAndAsmaReturnsThroughAllJniCallPaths) {
  VM vm;
  JNIEnv* env = vm.GetJNIEnv();
  ASSERT_NE(env, nullptr);
  jclass helper_class =
      env->FindClass("com/roblox/client/startup/NativeHelper");
  ASSERT_NE(helper_class, nullptr);
  jobject helper = env->AllocObject(helper_class);
  ASSERT_NE(helper, nullptr);
  jmethodID did_return = env->GetMethodID(
      helper_class, "gameActivity_onLuaAppDidReturn", "()V");
  ASSERT_NE(did_return, nullptr);
  jclass native_gl_class =
      env->FindClass("com/roblox/engine/jni/NativeGLJavaInterface");
  ASSERT_NE(native_gl_class, nullptr);
  jmethodID game_did_leave =
      env->GetStaticMethodID(native_gl_class, "gameDidLeave", "()V");
  ASSERT_NE(game_did_leave, nullptr);

  auto count = std::make_shared<std::atomic<int>>(0);
  vm.SetRobloxExperienceLifecycleCallbacks(
      count, RobloxExperienceLifecycleCallbacks{&IncrementReturnCount});

  env->CallVoidMethod(helper, did_return);
  CallVoidMethodVForTest(env, helper, did_return);
  env->CallVoidMethodA(helper, did_return, nullptr);
  env->CallStaticVoidMethod(native_gl_class, game_did_leave);
  CallStaticVoidMethodVForTest(env, native_gl_class, game_did_leave);
  env->CallStaticVoidMethodA(native_gl_class, game_did_leave, nullptr);
  EXPECT_EQ(count->load(), 6);

  jmethodID wrong_signature = env->GetMethodID(
      helper_class, "gameActivity_onLuaAppDidReturn", "(I)V");
  env->CallVoidMethod(helper, wrong_signature, 1);
  jclass wrong_class = env->FindClass("java/lang/Object");
  jobject wrong_receiver = env->AllocObject(wrong_class);
  env->CallVoidMethod(wrong_receiver, did_return);
  jmethodID wrong_static_signature =
      env->GetStaticMethodID(native_gl_class, "gameDidLeave", "(I)V");
  env->CallStaticVoidMethod(native_gl_class, wrong_static_signature, 1);
  jmethodID wrong_static_class =
      env->GetStaticMethodID(wrong_class, "gameDidLeave", "()V");
  env->CallStaticVoidMethod(wrong_class, wrong_static_class);
  EXPECT_EQ(count->load(), 6);

  vm.ClearRobloxExperienceLifecycleCallbacks();
  env->CallVoidMethod(helper, did_return);
  env->CallStaticVoidMethod(native_gl_class, game_did_leave);
  EXPECT_EQ(count->load(), 6);
}

struct BlockingReturnProbe {
  std::mutex mutex;
  std::condition_variable entered;
  std::condition_variable released;
  bool is_entered = false;
  bool may_return = false;
};

void BlockReturn(void* context) {
  auto* probe = static_cast<BlockingReturnProbe*>(context);
  std::unique_lock<std::mutex> lock(probe->mutex);
  probe->is_entered = true;
  probe->entered.notify_all();
  probe->released.wait(lock, [probe] { return probe->may_return; });
}

TEST(JniVmExperienceLifecycleTest, ClearRetainsInflightOwnedContext) {
  VM vm;
  JNIEnv* env = vm.GetJNIEnv();
  jclass helper_class =
      env->FindClass("com/roblox/client/startup/NativeHelper");
  jobject helper = env->AllocObject(helper_class);
  jmethodID did_return = env->GetMethodID(
      helper_class, "gameActivity_onLuaAppDidReturn", "()V");

  auto probe = std::make_shared<BlockingReturnProbe>();
  std::weak_ptr<BlockingReturnProbe> weak_probe = probe;
  vm.SetRobloxExperienceLifecycleCallbacks(
      probe, RobloxExperienceLifecycleCallbacks{&BlockReturn});

  std::thread worker(
      [env, helper, did_return] { env->CallVoidMethod(helper, did_return); });
  {
    std::unique_lock<std::mutex> lock(probe->mutex);
    ASSERT_TRUE(probe->entered.wait_for(
        lock, std::chrono::seconds(2),
        [probe] { return probe->is_entered; }));
  }

  vm.ClearRobloxExperienceLifecycleCallbacks();
  probe.reset();
  EXPECT_FALSE(weak_probe.expired());
  {
    std::shared_ptr<BlockingReturnProbe> retained = weak_probe.lock();
    ASSERT_NE(retained, nullptr);
    std::lock_guard<std::mutex> lock(retained->mutex);
    retained->may_return = true;
    retained->released.notify_all();
  }
  worker.join();
  EXPECT_TRUE(weak_probe.expired());
  EXPECT_FALSE(vm.DispatchRobloxExperienceLuaAppDidReturn());
}

}  // namespace
}  // namespace jnivm
