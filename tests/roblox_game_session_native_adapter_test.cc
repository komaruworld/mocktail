#include "runtime/roblox_game_session_native_adapter.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#include "jnivm/jnivm.h"

namespace mocktail {
namespace runtime {
namespace {

const std::vector<std::string>& RequiredSymbolNames() {
  static const std::vector<std::string> names = {
      "Java_com_roblox_engine_jni_NativeGLInterface_"
      "setTaskSchedulerBackgroundMode",
      "Java_com_roblox_engine_jni_NativeGLInterface_"
      "nativeAppBridgeV2StartGameWithParam",
      "Java_com_roblox_engine_jni_NativeGLInterface_"
      "nativeAppBridgeV2UpdateSurfaceGameWithPlatformParams",
      "Java_com_roblox_engine_jni_NativeGLInterface_"
      "nativeAppBridgeV2PauseGame",
      "Java_com_roblox_engine_jni_NativeGLInterface_"
      "nativeAppBridgeV2ResumeGameWithPlatformParams",
      "Java_com_roblox_engine_jni_NativeGLInterface_"
      "nativeAppBridgeV2LeaveGame",
      "Java_com_roblox_engine_jni_NativeGLInterface_"
      "nativeAppBridgeV2PauseApp",
      "Java_com_roblox_engine_jni_NativeGLInterface_"
      "nativeAppBridgeV2DestroyApp",
      "Java_com_roblox_engine_jni_NativeGLInterface_"
      "nativeAppBridgeV2UpdateSurfaceAppWithPlatformParams",
      "Java_com_roblox_engine_jni_NativeGLInterface_"
      "nativeAppBridgeV2StartAppWithParams",
      "Java_com_roblox_engine_jni_NativeGLInterface_"
      "nativeCallMessagesFromMainThread",
  };
  return names;
}

class FakeSymbolTable {
 public:
  RobloxSymbolLookup Lookup() {
    return RobloxSymbolLookup(&FakeSymbolTable::Find, this);
  }

  void Hide(std::string name) { hidden_.insert(std::move(name)); }
  const std::vector<std::string>& requests() const { return requests_; }

 private:
  static void* Find(void* context, const char* name) {
    auto* table = static_cast<FakeSymbolTable*>(context);
    if (table == nullptr || name == nullptr) {
      return nullptr;
    }
    table->requests_.emplace_back(name);
    if (table->hidden_.find(name) != table->hidden_.end()) {
      return nullptr;
    }
    return table;
  }

  std::unordered_set<std::string> hidden_;
  std::vector<std::string> requests_;
};

struct Probe {
  std::vector<std::string> calls;
  std::vector<JNIEnv*> environments;
  std::vector<jobject> update_surfaces;
  std::vector<jobject> update_platform_params;
  std::vector<jobject> resume_surfaces;
  std::vector<jobject> resume_platform_params;
  std::vector<jobject> deleted_global_refs;
  std::string foreground_reason;
  int prepare_count = 0;
  int new_global_ref_count = 0;
  int fail_new_global_ref_at = 0;
  bool foreground_enabled = true;
  RobloxGameSessionRuntime* runtime = nullptr;
  bool present_during_start = false;
  bool update_raises_exception = false;
  bool exception_pending = false;
  bool block_update = false;
  bool update_entered = false;
  bool release_update = false;
  std::mutex update_mutex;
  std::condition_variable update_condition;
  bool block_pause = false;
  bool release_pause_on_pump = false;
  bool pause_released = false;
  std::atomic<int> message_pump_count{0};
  std::atomic<int> presented_notifications{0};
  std::atomic<uint64_t> presented_frame{0};
  std::mutex pause_mutex;
  std::condition_variable pause_condition;
  const JNINativeInterface_* base_functions = nullptr;
  JNINativeInterface_ proxy_functions{};
};

Probe* g_probe = nullptr;

void ObserveGamePresented(void* context, uint64_t frame_serial) {
  auto* probe = static_cast<Probe*>(context);
  probe->presented_frame.store(frame_serial);
  ++probe->presented_notifications;
}

void Record(JNIEnv* env, const char* call) {
  ASSERT_NE(g_probe, nullptr);
  g_probe->calls.emplace_back(call);
  g_probe->environments.push_back(env);
}

void SetBackground(JNIEnv* env, jclass, jboolean enabled, jstring reason) {
  Record(env, enabled == JNI_TRUE ? "background" : "foreground");
  g_probe->foreground_enabled = enabled == JNI_TRUE;
  const char* chars = env->GetStringUTFChars(reason, nullptr);
  g_probe->foreground_reason = chars != nullptr ? chars : "";
  env->ReleaseStringUTFChars(reason, chars);
}

jint StartGame(JNIEnv* env, jclass, jobject) {
  Record(env, "start");
  if (g_probe->present_during_start && g_probe->runtime != nullptr) {
    EXPECT_TRUE(g_probe->runtime->RecordPresent(7).ok());
  }
  return 1;
}

void UpdateSurface(JNIEnv* env, jclass, jobject surface,
                   jobject platform_params, jobject) {
  Record(env, "update");
  g_probe->update_surfaces.push_back(surface);
  g_probe->update_platform_params.push_back(platform_params);
  if (g_probe->update_raises_exception) {
    g_probe->exception_pending = true;
  }
  std::unique_lock<std::mutex> lock(g_probe->update_mutex);
  if (g_probe->block_update) {
    g_probe->update_entered = true;
    g_probe->update_condition.notify_all();
    g_probe->update_condition.wait_for(lock, std::chrono::seconds(2), [] {
      return g_probe != nullptr && g_probe->release_update;
    });
  }
}

void PauseGame(JNIEnv* env, jclass) {
  Record(env, "pause-game");
  std::unique_lock<std::mutex> lock(g_probe->pause_mutex);
  if (g_probe->block_pause) {
    g_probe->pause_condition.wait_for(lock, std::chrono::seconds(2), [] {
      return g_probe != nullptr && g_probe->pause_released;
    });
  }
}

void ResumeGame(JNIEnv* env, jclass, jobject surface, jobject platform_params,
                jobject) {
  Record(env, "resume");
  g_probe->resume_surfaces.push_back(surface);
  g_probe->resume_platform_params.push_back(platform_params);
}

void LeaveGame(JNIEnv* env, jclass) { Record(env, "leave"); }

void PauseApp(JNIEnv* env, jclass) { Record(env, "pause-app"); }

void DestroyApp(JNIEnv* env, jclass) { Record(env, "destroy-app"); }

void UpdateAppSurface(JNIEnv* env, jclass, jobject, jobject) {
  Record(env, "update-app");
}

void StartApp(JNIEnv* env, jclass, jobject) { Record(env, "start-app"); }

void CallMessagesFromMainThread(JNIEnv*, jclass) {
  g_probe->message_pump_count.fetch_add(1, std::memory_order_relaxed);
  if (!g_probe->release_pause_on_pump) {
    return;
  }
  {
    std::lock_guard<std::mutex> lock(g_probe->pause_mutex);
    g_probe->pause_released = true;
  }
  g_probe->pause_condition.notify_all();
}

jobject ProxyNewGlobalRef(JNIEnv* env, jobject object) {
  ++g_probe->new_global_ref_count;
  if (g_probe->fail_new_global_ref_at == g_probe->new_global_ref_count) {
    return nullptr;
  }
  return g_probe->base_functions->NewGlobalRef(env, object);
}

void ProxyDeleteGlobalRef(JNIEnv* env, jobject object) {
  g_probe->deleted_global_refs.push_back(object);
  g_probe->base_functions->DeleteGlobalRef(env, object);
}

jboolean ProxyExceptionCheck(JNIEnv*) {
  return g_probe->exception_pending ? JNI_TRUE : JNI_FALSE;
}

void ProxyExceptionClear(JNIEnv*) { g_probe->exception_pending = false; }

void PrepareEnvironment(void* context) {
  auto* pair = static_cast<std::pair<jnivm::VM*, Probe*>*>(context);
  ASSERT_NE(pair, nullptr);
  pair->first->RestoreFunctions();
  JNIEnv* env = pair->first->GetJNIEnv();
  pair->second->base_functions = env->functions;
  pair->second->proxy_functions = *env->functions;
  pair->second->proxy_functions.NewGlobalRef = ProxyNewGlobalRef;
  pair->second->proxy_functions.DeleteGlobalRef = ProxyDeleteGlobalRef;
  pair->second->proxy_functions.ExceptionCheck = ProxyExceptionCheck;
  pair->second->proxy_functions.ExceptionClear = ProxyExceptionClear;
  env->functions = &pair->second->proxy_functions;
  ++pair->second->prepare_count;
}

RobloxGameSessionSymbols Symbols() {
  return {SetBackground,
          StartGame,
          UpdateSurface,
          PauseGame,
          ResumeGame,
          LeaveGame,
          PauseApp,
          DestroyApp,
          UpdateAppSurface,
          StartApp,
          CallMessagesFromMainThread};
}

GameSessionPrincipal AuthenticatedPrincipal() {
  return {GameSessionPrincipalKind::kAuthenticated, 11, "123456",
          "https://www.roblox.com"};
}

GameSessionPrincipal LocalGuestPrincipal() {
  return {
      GameSessionPrincipalKind::kLocalGuest, 11, {}, "https://www.roblox.com"};
}

GameJoinRequest NetworkRequest() { return {22, 123, "network-launch"}; }

GameJoinRequest LocalRequest() { return {22, 0, "local-launch"}; }

GameSurface Surface(uint64_t generation = 33, uintptr_t window = 0x44,
                    uint32_t width = 1280, uint32_t height = 720) {
  return {generation, window, width, height};
}

class RobloxGameSessionNativeAdapterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    g_probe = &probe_;
    vm_ = std::make_unique<jnivm::VM>();
    vm_->RegisterClass("com/roblox/engine/jni/NativeGLInterface");
    vm_->RegisterClass("android/view/Surface");
    vm_->RegisterClass("com/roblox/engine/jni/model/PlatformParams");
    vm_->RegisterClass("android/app/Activity");
    vm_->RegisterClass("com/roblox/engine/jni/autovalue/StartGameParams");
    environment_context_ = {vm_.get(), &probe_};
  }

  void TearDown() override {
    if (vm_ != nullptr) {
      vm_->RestoreFunctions();
    }
    g_probe = nullptr;
  }

  JniEnvironmentProvider Environment() {
    return {vm_->GetJavaVM(), &environment_context_, PrepareEnvironment};
  }

  RobloxGameSessionBinding Binding(int64_t place_id = 123) {
    JNIEnv* env = vm_->GetJNIEnv();
    RobloxGameSessionBinding binding;
    binding.objects.native_gl_class =
        env->FindClass("com/roblox/engine/jni/NativeGLInterface");
    binding.objects.surface =
        env->AllocObject(env->FindClass("android/view/Surface"));
    binding.objects.platform_params = env->AllocObject(
        env->FindClass("com/roblox/engine/jni/model/PlatformParams"));
    binding.objects.activity =
        env->AllocObject(env->FindClass("android/app/Activity"));
    binding.objects.start_game_params = env->AllocObject(
        env->FindClass("com/roblox/engine/jni/autovalue/StartGameParams"));
    binding.principal =
        place_id == 0 ? LocalGuestPrincipal() : AuthenticatedPrincipal();
    binding.request = place_id == 0
                          ? LocalRequest()
                          : GameJoinRequest{22, place_id, "network-launch"};
    binding.surface = Surface();
    return binding;
  }

  Probe probe_;
  std::unique_ptr<jnivm::VM> vm_;
  std::pair<jnivm::VM*, Probe*> environment_context_;
};

TEST(RobloxGameSessionResolverTest, InvalidLookupFailsClosed) {
  const RobloxGameSessionResolution resolution =
      ResolveRobloxGameSessionSymbols(RobloxSymbolLookup());

  EXPECT_FALSE(resolution.ok());
  EXPECT_EQ(resolution.status(),
            RobloxGameSessionResolutionStatus::kInvalidLookup);
  EXPECT_TRUE(resolution.missing_required_symbols().empty());
}

TEST(RobloxGameSessionResolverTest, ResolvesExactRequiredSet) {
  FakeSymbolTable table;

  const RobloxGameSessionResolution resolution =
      ResolveRobloxGameSessionSymbols(table.Lookup());

  ASSERT_TRUE(resolution.ok());
  ASSERT_NE(resolution.symbols(), nullptr);
  EXPECT_TRUE(resolution.symbols()->complete());
  EXPECT_EQ(table.requests(), RequiredSymbolNames());
}

TEST(RobloxGameSessionResolverTest, EveryMissingSymbolFailsClosed) {
  for (const std::string& missing : RequiredSymbolNames()) {
    SCOPED_TRACE(missing);
    FakeSymbolTable table;
    table.Hide(missing);

    const RobloxGameSessionResolution resolution =
        ResolveRobloxGameSessionSymbols(table.Lookup());

    EXPECT_FALSE(resolution.ok());
    EXPECT_EQ(resolution.status(),
              RobloxGameSessionResolutionStatus::kMissingRequiredSymbols);
    ASSERT_EQ(resolution.missing_required_symbols().size(), 1U);
    EXPECT_EQ(resolution.missing_required_symbols().front(), missing);
  }
}

TEST_F(RobloxGameSessionNativeAdapterTest,
       InitialLocalGameUsesExactApkOrderWithoutUpdateOrResume) {
  RobloxGameSessionRuntime runtime(Environment(), Symbols());
  probe_.runtime = &runtime;

  const Status status = runtime.InitializeAndStart(
      Binding(0), LocalGuestPrincipal(), LocalRequest(), Surface());

  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_EQ(probe_.calls, std::vector<std::string>({"foreground", "start"}));
  EXPECT_FALSE(probe_.foreground_enabled);
  EXPECT_EQ(probe_.foreground_reason, "ES.onSurfaceCreated");
  EXPECT_EQ(runtime.Snapshot().state, GameSessionState::kRunning);
  EXPECT_GE(probe_.prepare_count, 3);
}

TEST_F(RobloxGameSessionNativeAdapterTest,
       PresentDuringStartIsRetainedAfterNativeSuccess) {
  RobloxGameSessionRuntime runtime(Environment(), Symbols());
  probe_.runtime = &runtime;
  probe_.present_during_start = true;

  const Status status = runtime.InitializeAndStart(
      Binding(), AuthenticatedPrincipal(), NetworkRequest(), Surface());

  ASSERT_TRUE(status.ok()) << status.message();
  EXPECT_TRUE(runtime.Snapshot().game_presented);
  EXPECT_EQ(runtime.Snapshot().first_presented_frame, 7U);
}

TEST_F(RobloxGameSessionNativeAdapterTest,
       PresentObserverLatchesOnlyAfterAcceptedGameEvidence) {
  RobloxGameSessionRuntime runtime(Environment(), Symbols(), {},
                                   {&probe_, ObserveGamePresented});
  probe_.runtime = &runtime;

  RobloxGameSessionRuntime::SuccessfulPresentCallback(&runtime, 3);
  EXPECT_FALSE(runtime.Snapshot().game_presented);
  EXPECT_EQ(probe_.presented_notifications.load(), 0);

  ASSERT_TRUE(runtime
                  .InitializeAndStart(Binding(), AuthenticatedPrincipal(),
                                      NetworkRequest(), Surface())
                  .ok());
  RobloxGameSessionRuntime::SuccessfulPresentCallback(&runtime, 5);
  RobloxGameSessionRuntime::SuccessfulPresentCallback(&runtime, 7);

  EXPECT_TRUE(runtime.Snapshot().game_presented);
  EXPECT_EQ(runtime.Snapshot().first_presented_frame, 5U);
  EXPECT_EQ(probe_.presented_notifications.load(), 1);
  EXPECT_EQ(probe_.presented_frame.load(), 5U);
}

TEST_F(RobloxGameSessionNativeAdapterTest,
       SameGenerationResizeReusesSurfaceAndReplacesPlatformParams) {
  RobloxGameSessionBinding binding = Binding();
  const jobject initial_surface = binding.objects.surface;
  const jobject initial_params = binding.objects.platform_params;
  RobloxGameSurfaceJniConfig surface_config;
  surface_config.asset_folder_path = "/typed/assets";
  surface_config.dpi_scale = 1.5f;
  surface_config.viewport_width_mm = 300;
  surface_config.viewport_height_mm = 170;
  RobloxGameSessionRuntime runtime(Environment(), Symbols(), surface_config);
  ASSERT_TRUE(runtime
                  .InitializeAndStart(binding, AuthenticatedPrincipal(),
                                      NetworkRequest(), Surface())
                  .ok());

  const GameSessionUpdateResult first =
      runtime.SurfaceChanged(Surface(33, 0x44, 1600, 900));
  const GameSessionUpdateResult second =
      runtime.SurfaceChanged(Surface(33, 0x44, 1920, 1080));

  ASSERT_TRUE(first.ok()) << first.cause.message();
  ASSERT_TRUE(second.ok()) << second.cause.message();
  ASSERT_EQ(probe_.update_surfaces.size(), 2U);
  ASSERT_EQ(probe_.update_platform_params.size(), 2U);
  EXPECT_EQ(probe_.update_surfaces[0], initial_surface);
  EXPECT_EQ(probe_.update_surfaces[1], initial_surface);
  EXPECT_NE(probe_.update_platform_params[0], initial_params);
  EXPECT_NE(probe_.update_platform_params[1], probe_.update_platform_params[0]);
  JNIEnv* env = vm_->GetJNIEnv();
  jobject params = probe_.update_platform_params[1];
  jclass params_class = env->GetObjectClass(params);
  EXPECT_EQ(env->GetBooleanField(
                params, env->GetFieldID(params_class, "isTouchDevice", "Z")),
            JNI_FALSE);
  EXPECT_EQ(env->GetIntField(
                params, env->GetFieldID(params_class, "viewportWidthMm", "I")),
            300);
  EXPECT_FLOAT_EQ(env->GetFloatField(
                      params, env->GetFieldID(params_class, "dpiScale", "F")),
                  1.5f);
  jstring asset_path = static_cast<jstring>(env->GetObjectField(
      params,
      env->GetFieldID(params_class, "assetFolderPath", "Ljava/lang/String;")));
  const char* asset_chars = env->GetStringUTFChars(asset_path, nullptr);
  ASSERT_NE(asset_chars, nullptr);
  EXPECT_STREQ(asset_chars, "/typed/assets");
  env->ReleaseStringUTFChars(asset_path, asset_chars);
  EXPECT_EQ(runtime.Snapshot().active_surface_generation, 33U);
  EXPECT_EQ(runtime.Snapshot().active_surface_width, 1920U);
  EXPECT_EQ(runtime.Snapshot().active_surface_height, 1080U);
}

TEST_F(RobloxGameSessionNativeAdapterTest,
       DestroyAndRecreateReplacesSurfaceThenResumesWithoutLeave) {
  RobloxGameSessionBinding binding = Binding();
  const jobject initial_surface = binding.objects.surface;
  const jobject initial_params = binding.objects.platform_params;
  RobloxGameSessionRuntime runtime(Environment(), Symbols());
  ASSERT_TRUE(runtime
                  .InitializeAndStart(binding, AuthenticatedPrincipal(),
                                      NetworkRequest(), Surface())
                  .ok());
  ASSERT_TRUE(runtime.SurfaceDestroyed(33).ok());
  ASSERT_TRUE(runtime.SurfaceCreated(34).ok());
  const GameSessionUpdateResult recreate =
      runtime.SurfaceChanged(Surface(34, 0x55));
  ASSERT_TRUE(recreate.ok()) << recreate.cause.message();
  ASSERT_EQ(probe_.resume_surfaces.size(), 1U);
  ASSERT_EQ(probe_.resume_platform_params.size(), 1U);
  EXPECT_NE(probe_.resume_surfaces[0], initial_surface);
  EXPECT_NE(probe_.resume_platform_params[0], initial_params);
  JNIEnv* env = vm_->GetJNIEnv();
  jclass surface_class = env->GetObjectClass(probe_.resume_surfaces[0]);
  EXPECT_EQ(env->GetIntField(probe_.resume_surfaces[0],
                             env->GetFieldID(surface_class, "width", "I")),
            1280);
  EXPECT_EQ(
      env->GetBooleanField(probe_.resume_surfaces[0],
                           env->GetFieldID(surface_class, "isValid", "Z")),
      JNI_TRUE);
  EXPECT_EQ(runtime.Snapshot().active_surface_generation, 34U);

  const GameSessionUpdateResult shutdown = runtime.Shutdown();

  EXPECT_TRUE(shutdown.ok());
  EXPECT_EQ(shutdown.state, GameSessionState::kStopped);
  EXPECT_EQ(probe_.calls, std::vector<std::string>(
                              {"foreground", "start", "pause-game",
                               "foreground", "resume", "pause-game", "leave",
                               "pause-app", "background", "destroy-app"}));
  EXPECT_EQ(probe_.foreground_reason, "ASMA.stop");
  EXPECT_LT(std::find(probe_.calls.begin(), probe_.calls.end(), "leave"),
            std::find(probe_.calls.begin(), probe_.calls.end(), "pause-app"));
}

TEST_F(RobloxGameSessionNativeAdapterTest,
       ShutdownPumpsMainThreadMessagesWhilePauseDrainsRenderJobs) {
  RobloxGameSessionRuntime runtime(Environment(), Symbols());
  ASSERT_TRUE(runtime
                  .InitializeAndStart(Binding(), AuthenticatedPrincipal(),
                                      NetworkRequest(), Surface())
                  .ok());
  probe_.block_pause = true;
  probe_.release_pause_on_pump = true;

  const GameSessionUpdateResult shutdown = runtime.Shutdown();

  EXPECT_TRUE(shutdown.ok()) << shutdown.cause.message();
  EXPECT_EQ(shutdown.state, GameSessionState::kStopped);
  EXPECT_TRUE(probe_.pause_released);
  EXPECT_GT(probe_.message_pump_count.load(std::memory_order_relaxed), 0);
}

TEST_F(RobloxGameSessionNativeAdapterTest,
       SameGenerationNativeHandleReplacementFailsBeforeJni) {
  RobloxGameSessionNativeAdapter adapter(Environment(), Symbols());
  ASSERT_TRUE(adapter.Initialize(Binding()).ok());
  const GameSessionCapabilities capabilities = adapter.Capabilities();
  ASSERT_TRUE(capabilities
                  .start_game(capabilities.context, AuthenticatedPrincipal(),
                              NetworkRequest(), Surface())
                  .ok());

  const Status status = capabilities.update_surface(
      capabilities.context, Surface(33, 0x55, 1600, 900));

  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.code(), StatusCode::kFailedPrecondition);
  EXPECT_EQ(probe_.calls, std::vector<std::string>({"start"}));
}

TEST_F(RobloxGameSessionNativeAdapterTest,
       FailedNativeRebindDeletesCandidateAndRetainsLastCommittedRefs) {
  RobloxGameSessionBinding binding = Binding();
  const jobject initial_params = binding.objects.platform_params;
  RobloxGameSessionNativeAdapter adapter(Environment(), Symbols());
  ASSERT_TRUE(adapter.Initialize(binding).ok());
  const GameSessionCapabilities capabilities = adapter.Capabilities();
  ASSERT_TRUE(capabilities
                  .start_game(capabilities.context, AuthenticatedPrincipal(),
                              NetworkRequest(), Surface())
                  .ok());
  ASSERT_TRUE(
      capabilities
          .update_surface(capabilities.context, Surface(33, 0x44, 1600, 900))
          .ok());
  ASSERT_EQ(probe_.update_platform_params.size(), 1U);
  const jobject committed_params = probe_.update_platform_params[0];
  EXPECT_NE(committed_params, initial_params);
  EXPECT_EQ(std::count(probe_.deleted_global_refs.begin(),
                       probe_.deleted_global_refs.end(), initial_params),
            1);

  probe_.update_raises_exception = true;
  const Status failed = capabilities.update_surface(
      capabilities.context, Surface(33, 0x44, 1920, 1080));
  probe_.update_raises_exception = false;

  ASSERT_FALSE(failed.ok());
  EXPECT_EQ(failed.code(), StatusCode::kPlatformError);
  ASSERT_EQ(probe_.update_platform_params.size(), 2U);
  const jobject failed_candidate = probe_.update_platform_params[1];
  EXPECT_EQ(std::count(probe_.deleted_global_refs.begin(),
                       probe_.deleted_global_refs.end(), failed_candidate),
            1);
  EXPECT_EQ(std::count(probe_.deleted_global_refs.begin(),
                       probe_.deleted_global_refs.end(), committed_params),
            0);

  ASSERT_TRUE(adapter.Release().ok());
  EXPECT_EQ(std::count(probe_.deleted_global_refs.begin(),
                       probe_.deleted_global_refs.end(), committed_params),
            1);
}

TEST_F(RobloxGameSessionNativeAdapterTest,
       GlobalRefAllocationFailureRollsBackWithoutNativeCallback) {
  RobloxGameSessionNativeAdapter adapter(Environment(), Symbols());
  ASSERT_TRUE(adapter.Initialize(Binding()).ok());
  const GameSessionCapabilities capabilities = adapter.Capabilities();
  ASSERT_TRUE(capabilities
                  .start_game(capabilities.context, AuthenticatedPrincipal(),
                              NetworkRequest(), Surface())
                  .ok());
  probe_.fail_new_global_ref_at = probe_.new_global_ref_count + 1;

  const Status update = capabilities.update_surface(
      capabilities.context, Surface(33, 0x44, 1600, 900));

  EXPECT_FALSE(update.ok());
  EXPECT_EQ(update.code(), StatusCode::kUnavailable);
  EXPECT_EQ(probe_.calls, std::vector<std::string>({"start"}));
  EXPECT_TRUE(adapter.initialized());
  probe_.fail_new_global_ref_at = 0;
  EXPECT_TRUE(adapter.Release().ok());
}

TEST_F(RobloxGameSessionNativeAdapterTest,
       ShutdownWaitsForRebindAndNeverDeletesInFlightReferences) {
  RobloxGameSessionRuntime runtime(Environment(), Symbols());
  ASSERT_TRUE(runtime
                  .InitializeAndStart(Binding(), AuthenticatedPrincipal(),
                                      NetworkRequest(), Surface())
                  .ok());
  probe_.block_update = true;
  GameSessionUpdateResult resize_result;
  std::thread resize_thread([&] {
    resize_result = runtime.SurfaceChanged(Surface(33, 0x44, 1600, 900));
  });
  {
    std::unique_lock<std::mutex> lock(probe_.update_mutex);
    ASSERT_TRUE(probe_.update_condition.wait_for(
        lock, std::chrono::seconds(2),
        [this] { return probe_.update_entered; }));
  }
  const std::size_t deletes_while_blocked = probe_.deleted_global_refs.size();
  std::atomic<bool> shutdown_thread_started{false};
  std::atomic<bool> shutdown_finished{false};
  GameSessionUpdateResult shutdown_result;
  std::thread shutdown_thread([&] {
    shutdown_thread_started.store(true, std::memory_order_release);
    shutdown_result = runtime.Shutdown();
    shutdown_finished.store(true, std::memory_order_release);
  });
  while (!shutdown_thread_started.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  EXPECT_FALSE(shutdown_finished.load(std::memory_order_acquire));
  EXPECT_EQ(probe_.deleted_global_refs.size(), deletes_while_blocked);
  {
    std::lock_guard<std::mutex> lock(probe_.update_mutex);
    probe_.release_update = true;
  }
  probe_.update_condition.notify_all();
  resize_thread.join();
  shutdown_thread.join();

  EXPECT_TRUE(resize_result.ok());
  EXPECT_TRUE(shutdown_result.ok());
  EXPECT_EQ(shutdown_result.state, GameSessionState::kStopped);
  EXPECT_EQ(probe_.calls,
            std::vector<std::string>({"foreground", "start", "update",
                                      "pause-game", "leave", "pause-app",
                                      "background", "destroy-app"}));
}

TEST_F(RobloxGameSessionNativeAdapterTest,
       BindingMismatchFailsBeforeNativeStart) {
  RobloxGameSessionRuntime runtime(Environment(), Symbols());
  RobloxGameSessionBinding binding = Binding();
  binding.request.request_id = 99;

  const Status status = runtime.InitializeAndStart(
      binding, AuthenticatedPrincipal(), NetworkRequest(), Surface());

  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.code(), StatusCode::kFailedPrecondition);
  EXPECT_EQ(probe_.calls, std::vector<std::string>({"foreground"}));
  EXPECT_EQ(runtime.Snapshot().state, GameSessionState::kFailure);
}

TEST_F(RobloxGameSessionNativeAdapterTest,
       EveryInitialBindingFieldMustMatchBeforeNativeStart) {
  const auto expect_rejected = [this](RobloxGameSessionBinding binding) {
    SCOPED_TRACE(binding.request.request_id);
    probe_.calls.clear();
    RobloxGameSessionRuntime runtime(Environment(), Symbols());

    const Status status = runtime.InitializeAndStart(
        binding, AuthenticatedPrincipal(), NetworkRequest(), Surface());

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), StatusCode::kFailedPrecondition);
    EXPECT_EQ(std::find(probe_.calls.begin(), probe_.calls.end(), "start"),
              probe_.calls.end());
  };

  RobloxGameSessionBinding principal_id = Binding();
  principal_id.principal.principal_id = "different-principal";
  expect_rejected(std::move(principal_id));

  RobloxGameSessionBinding principal_kind = Binding();
  principal_kind.principal.kind = GameSessionPrincipalKind::kLocalGuest;
  principal_kind.principal.principal_id.clear();
  expect_rejected(std::move(principal_kind));

  RobloxGameSessionBinding principal_generation = Binding();
  principal_generation.principal.generation = 12;
  expect_rejected(std::move(principal_generation));

  RobloxGameSessionBinding base_url = Binding();
  base_url.principal.base_url = "https://example.invalid";
  expect_rejected(std::move(base_url));

  RobloxGameSessionBinding launch_parameters = Binding();
  launch_parameters.request.launch_parameters = "different-launch";
  expect_rejected(std::move(launch_parameters));

  RobloxGameSessionBinding place = Binding();
  place.request.place_id = 456;
  expect_rejected(std::move(place));

  RobloxGameSessionBinding surface_window = Binding();
  surface_window.surface.native_window = 0x55;
  expect_rejected(std::move(surface_window));

  RobloxGameSessionBinding surface_generation = Binding();
  surface_generation.surface.generation = 34;
  expect_rejected(std::move(surface_generation));

  RobloxGameSessionBinding surface_width = Binding();
  surface_width.surface.width = 1600;
  expect_rejected(std::move(surface_width));

  RobloxGameSessionBinding surface_height = Binding();
  surface_height.surface.height = 900;
  expect_rejected(std::move(surface_height));
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
