#include "runtime/roblox_experience_composition.h"

#include <gtest/gtest.h>
#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "jnivm/jnivm.h"
#include "runtime/external_launch_broker.h"

namespace mocktail {
namespace runtime {
namespace {

struct Probe {
  explicit Probe(jnivm::VM* source_vm) : vm(source_vm) {}

  jnivm::VM* vm = nullptr;
  jobject callback = nullptr;
  FreshLaunchPresentObserver observer = nullptr;
  void* observer_context = nullptr;
  int64_t started_place = 0;
  std::atomic<int> starts{0};
  int leaves = 0;
  int destroys = 0;
  int updates = 0;
  int app_updates = 0;
  int app_starts = 0;
  int app_pauses = 0;
  int app_surface_width = 0;
  int app_start_surface_width = 0;
  int64_t app_user_id = 0;
  std::vector<std::string> app_surface_lifecycle;
  int disconnects = 0;
  int surface_width = 0;
  int join_request_type = -1;
  int web_view_initializations = 0;
  int browser_bindings = 0;
  int browser_disconnects = 0;
  int browser_releases = 0;
  int browser_callbacks_created = 0;
  int browser_callbacks_cleared = 0;
  std::string web_view_javascript_callback;
  bool publish_during_subscribe = false;
  std::atomic<bool> block_start{false};
  std::atomic<bool> start_entered{false};
  std::atomic<bool> release_start{false};
  GameSurface observed_surface;
  std::vector<RobloxExperiencePresencePhase> presence_phases;
  std::vector<int64_t> presence_places;
  std::atomic<int> playing_presence_notifications{0};
};

Probe* g_probe = nullptr;

void Prepare(void* context) {
  static_cast<jnivm::VM*>(context)->RestoreFunctions();
}

GameSurface SnapshotSurface(void* context) {
  return static_cast<Probe*>(context)->observed_surface;
}

jnivm::RobloxCredentialView ProvideCredential(const void* context) {
  const auto* credential = static_cast<const std::string*>(context);
  return credential != nullptr ? jnivm::RobloxCredentialView{credential->data(),
                                                             credential->size()}
                               : jnivm::RobloxCredentialView{};
}

jobject CreateRawCallback(void* context, std::shared_ptr<void> callback_context,
                          void (*run)(void*, JNIEnv*, jstring)) {
  auto* probe = static_cast<Probe*>(context);
  return probe->vm->CreateMessageBusRawCallback(
      std::move(callback_context), jnivm::MessageBusRawCallbacks{run});
}

void ClearRawCallback(void* context, jobject callback) {
  static_cast<Probe*>(context)->vm->ClearMessageBusRawCallback(callback);
}

jobject CreateRequestHandler(void* context,
                             std::shared_ptr<void> callback_context,
                             std::string (*run)(void*, JNIEnv*, jstring)) {
  return static_cast<Probe*>(context)->vm->CreateMessageBusRequestHandler(
      std::move(callback_context),
      jnivm::MessageBusRequestHandlerCallbacks{run});
}

void ClearRequestHandler(void* context, jobject handler) {
  static_cast<Probe*>(context)->vm->ClearMessageBusRequestHandler(handler);
}

jobject CreateMemStorageCallback(void* context,
                                 std::shared_ptr<void> callback_context,
                                 void (*on_item_set)(void*, JNIEnv*, jstring)) {
  auto* probe = static_cast<Probe*>(context);
  ++probe->browser_callbacks_created;
  return probe->vm->CreateMemStorageCallback(
      std::move(callback_context),
      jnivm::MemStorageCallbackCallbacks{on_item_set});
}

void ClearMemStorageCallback(void* context, jobject callback) {
  auto* probe = static_cast<Probe*>(context);
  ++probe->browser_callbacks_cleared;
  probe->vm->ClearMemStorageCallback(callback);
}

bool SetPlatformWebCallbacks(
    void* context, std::shared_ptr<void> callback_context,
    void (*on_data_model_notification)(void*, JNIEnv*, jstring, jstring),
    void (*on_app_bridge_notification)(void*, JNIEnv*, jstring, jstring),
    void (*on_native_overlay)(void*, JNIEnv*, jstring, jstring),
    void (*on_open_web_activity)(void*, JNIEnv*, jstring, jstring),
    void (*on_sync_cookies)(void*, JNIEnv*, jstring),
    void (*on_set_cookie)(void*, JNIEnv*, jstring, jstring)) {
  auto* probe = static_cast<Probe*>(context);
  if (probe == nullptr || callback_context == nullptr ||
      on_data_model_notification == nullptr ||
      on_app_bridge_notification == nullptr || on_native_overlay == nullptr ||
      on_open_web_activity == nullptr || on_sync_cookies == nullptr ||
      on_set_cookie == nullptr) {
    return false;
  }
  probe->vm->SetRobloxDataModelNotificationCallbacks(
      std::move(callback_context),
      jnivm::RobloxDataModelNotificationCallbacks{
          on_data_model_notification, on_app_bridge_notification,
          on_native_overlay, on_open_web_activity, on_sync_cookies,
          on_set_cookie});
  return true;
}

void ClearPlatformWebCallbacks(void* context) {
  static_cast<Probe*>(context)->vm->ClearRobloxDataModelNotificationCallbacks();
}

jobject BindMemStorage(JNIEnv* env, jclass, jstring, jobject) {
  const jlong connection_ref = ++g_probe->browser_bindings;
  jclass connection_class =
      env->FindClass("com/roblox/engine/jni/memstorage/Connection");
  jmethodID constructor =
      connection_class != nullptr
          ? env->GetMethodID(connection_class, "<init>", "(J)V")
          : nullptr;
  jobject connection =
      constructor != nullptr
          ? env->NewObject(connection_class, constructor, connection_ref)
          : nullptr;
  if (connection_class != nullptr) {
    env->DeleteLocalRef(connection_class);
  }
  return connection;
}

void DisconnectMemStorage(JNIEnv*, jclass, jobject) {
  ++g_probe->browser_disconnects;
}

void ReleaseMemStorageConnection(JNIEnv*, jclass, jobject) {
  ++g_probe->browser_releases;
}

void FireMemStorage(JNIEnv*, jclass, jstring, jstring) {}

RobloxBrowserServiceSymbols BrowserServiceSymbols() {
  return {BindMemStorage, DisconnectMemStorage, ReleaseMemStorageConnection,
          FireMemStorage};
}

RobloxExperienceJniFactory JniFactory(Probe* probe) {
  return {probe,
          CreateRawCallback,
          ClearRawCallback,
          CreateRequestHandler,
          ClearRequestHandler,
          CreateMemStorageCallback,
          ClearMemStorageCallback,
          SetPlatformWebCallbacks,
          ClearPlatformWebCallbacks};
}

void SetRequestHandler(JNIEnv*, jobject, jstring, jstring, jobject) {}
void ClearNativeRequestHandler(JNIEnv*, jobject, jstring, jstring) {}
void PublishRaw(JNIEnv*, jobject, jstring, jstring) {}
void BroadcastDataModelFocus(JNIEnv*, jclass, jstring, jstring, jstring) {}

jstring GetLaunchId(JNIEnv* env, jclass) {
  return env->NewStringUTF("experienceLaunch");
}

jstring GetWebViewOpenId(JNIEnv* env, jclass) {
  return env->NewStringUTF("openWindow");
}

jstring GetWebViewMutateId(JNIEnv* env, jclass) {
  return env->NewStringUTF("mutateWindow");
}

jstring GetWebViewCloseId(JNIEnv* env, jclass) {
  return env->NewStringUTF("closeWindow");
}

jstring GetWebViewHandleWindowCloseId(JNIEnv* env, jclass) {
  return env->NewStringUTF("handleWindowClose");
}

jstring GetWebViewProtocolName(JNIEnv* env, jclass) {
  return env->NewStringUTF("WebView");
}

jstring GetWebViewAvailabilityId(JNIEnv* env, jclass) {
  return env->NewStringUTF("isAvailable");
}

jstring GetWebViewMessageId(JNIEnv* env, jclass, jstring protocol_name,
                            jstring method_name) {
  const char* protocol = env->GetStringUTFChars(protocol_name, nullptr);
  const char* method = env->GetStringUTFChars(method_name, nullptr);
  const std::string message_id =
      std::string(protocol != nullptr ? protocol : "") + "." +
      (method != nullptr ? method : "");
  if (protocol != nullptr) {
    env->ReleaseStringUTFChars(protocol_name, protocol);
  }
  if (method != nullptr) {
    env->ReleaseStringUTFChars(method_name, method);
  }
  return env->NewStringUTF(message_id.c_str());
}

void InitializeWebViewProtocol(JNIEnv*, jclass) {
  ++g_probe->web_view_initializations;
}

void SignalWebViewJavascriptCallback(JNIEnv* env, jclass, jstring payload) {
  const char* payload_chars = env->GetStringUTFChars(payload, nullptr);
  g_probe->web_view_javascript_callback =
      payload_chars != nullptr ? payload_chars : "";
  if (payload_chars != nullptr) {
    env->ReleaseStringUTFChars(payload, payload_chars);
  }
}

void UpdateCookieSetHandler(JNIEnv*, jobject, jobject) {}

jobject Subscribe(JNIEnv* env, jobject, jstring id, jobject callback,
                  jboolean) {
  const char* id_chars = env->GetStringUTFChars(id, nullptr);
  const bool experience =
      id_chars != nullptr && std::string(id_chars) == "experienceLaunch";
  if (id_chars != nullptr) {
    env->ReleaseStringUTFChars(id, id_chars);
  }
  if (experience) {
    g_probe->callback = callback;
  }
  if (experience && g_probe->publish_during_subscribe) {
    jclass callback_class = env->GetObjectClass(callback);
    jmethodID run =
        env->GetMethodID(callback_class, "run", "(Ljava/lang/String;)V");
    jstring request = env->NewStringUTF(
        R"({"placeId":17580461965,"gameJoinContext":"PlayButton"})");
    env->CallVoidMethod(callback, run, request);
  }
  jclass clazz =
      env->FindClass("com/roblox/universalapp/messagebus/Connection");
  jmethodID constructor = env->GetMethodID(clazz, "<init>", "(J)V");
  jobject connection =
      env->NewObject(clazz, constructor, static_cast<jlong>(91));
  env->DeleteLocalRef(clazz);
  return connection;
}

void Disconnect(JNIEnv*, jobject, jlong handle) {
  if (handle == 91) ++g_probe->disconnects;
}

void Foreground(JNIEnv* env, jclass, jboolean enabled, jstring reason) {
  if (reason == nullptr) return;
  const char* chars = env->GetStringUTFChars(reason, nullptr);
  const std::string value = chars != nullptr ? chars : "";
  if (chars != nullptr) env->ReleaseStringUTFChars(reason, chars);
  if (value == "ASMA.stop" || value == "ASMA.start") {
    g_probe->app_surface_lifecycle.push_back(
        std::string(enabled == JNI_TRUE ? "background:" : "foreground:") +
        value);
  }
}

jint Start(JNIEnv* env, jclass, jobject params) {
  ++g_probe->starts;
  g_probe->start_entered.store(true);
  jclass clazz = env->GetObjectClass(params);
  jfieldID place = env->GetFieldID(clazz, "placeId", "J");
  g_probe->started_place = env->GetLongField(params, place);
  g_probe->join_request_type =
      env->GetIntField(params, env->GetFieldID(clazz, "joinRequestType", "I"));
  jobject surface = env->GetObjectField(
      params, env->GetFieldID(clazz, "surface", "Landroid/view/Surface;"));
  jclass surface_class = env->GetObjectClass(surface);
  g_probe->surface_width =
      env->GetIntField(surface, env->GetFieldID(surface_class, "width", "I"));
  env->DeleteLocalRef(clazz);
  if (g_probe->observer != nullptr) {
    g_probe->observer(g_probe->observer_context, 17);
  }
  while (g_probe->block_start.load() && !g_probe->release_start.load()) {
    std::this_thread::yield();
  }
  return 1;
}

void Update(JNIEnv*, jclass, jobject, jobject, jobject) { ++g_probe->updates; }
void UpdateApp(JNIEnv* env, jclass, jobject surface, jobject) {
  ++g_probe->app_updates;
  jclass surface_class = env->GetObjectClass(surface);
  g_probe->app_surface_width =
      env->GetIntField(surface, env->GetFieldID(surface_class, "width", "I"));
  env->DeleteLocalRef(surface_class);
}
void PauseGame(JNIEnv*, jclass) {}
void CallMessagesFromMainThread(JNIEnv*, jclass) {}
void ResumeGame(JNIEnv*, jclass, jobject, jobject, jobject) {}
void Leave(JNIEnv*, jclass) { ++g_probe->leaves; }
void PauseApp(JNIEnv*, jclass) {
  ++g_probe->app_pauses;
  g_probe->app_surface_lifecycle.emplace_back("pause-app");
}
void DestroyApp(JNIEnv*, jclass) { ++g_probe->destroys; }

void StartApp(JNIEnv* env, jclass, jobject params) {
  ++g_probe->app_starts;
  g_probe->app_surface_lifecycle.emplace_back("start-app");
  jclass clazz = env->GetObjectClass(params);
  g_probe->app_user_id =
      env->GetLongField(params, env->GetFieldID(clazz, "appUserId", "J"));
  jobject surface = env->GetObjectField(
      params, env->GetFieldID(clazz, "surface", "Landroid/view/Surface;"));
  jclass surface_class = env->GetObjectClass(surface);
  g_probe->app_start_surface_width =
      env->GetIntField(surface, env->GetFieldID(surface_class, "width", "I"));
  env->DeleteLocalRef(surface_class);
  env->DeleteLocalRef(surface);
  env->DeleteLocalRef(clazz);
}

bool RegisterObserver(void* context, FreshLaunchPresentObserver observer,
                      void* observer_context) {
  auto* probe = static_cast<Probe*>(context);
  probe->observer = observer;
  probe->observer_context = observer_context;
  return true;
}

void ClearObserver(void* context) {
  auto* probe = static_cast<Probe*>(context);
  probe->observer = nullptr;
  probe->observer_context = nullptr;
}

void NotifyCompositionReturn(void* context) {
  static_cast<RobloxExperienceComposition*>(context)->NotifyLuaAppDidReturn();
}

void ObservePresence(void* context, RobloxExperiencePresencePhase phase,
                     const RobloxExperienceLaunchRequest* request) {
  auto* probe = static_cast<Probe*>(context);
  probe->presence_phases.push_back(phase);
  probe->presence_places.push_back(request != nullptr ? request->place_id : 0);
  if (phase == RobloxExperiencePresencePhase::kPlaying) {
    ++probe->playing_presence_notifications;
  }
}

void DrainUntilPresented(RobloxExperienceComposition* composition) {
  ASSERT_NE(composition, nullptr);
  for (int attempt = 0; attempt < 1000; ++attempt) {
    ASSERT_TRUE(composition->DrainLaunchRequests().ok());
    if (composition->Snapshot().game_presented) {
      ASSERT_TRUE(composition->DrainLaunchRequests().ok());
      return;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  FAIL() << "fresh launch worker did not present a frame";
}

TEST(RobloxExperienceCompositionTest,
     SubscribesAtLuaAppReadinessAndStartsFreshTypedGame) {
  jnivm::VM vm;
  Probe probe{&vm};
  g_probe = &probe;
  for (const char* class_name : {
           "com/roblox/engine/jni/NativeGLInterface",
           "android/view/Surface",
           "com/roblox/engine/jni/model/PlatformParams",
           "android/app/Activity",
           "com/roblox/engine/jni/autovalue/AutoValue_StartGameParams",
           "com/roblox/engine/jni/autovalue/StartAppParams",
           "com/roblox/universalapp/messagebus/MessageBus",
           "com/roblox/universalapp/messagebus/Connection",
           "com/roblox/engine/jni/memstorage/MemStorage",
           "com/roblox/engine/jni/memstorage/Connection",
           "com/roblox/engine/jni/memstorage/Callback",
       }) {
    vm.RegisterClass(class_name);
  }
  RobloxExperienceComposition composition(
      {vm.GetJavaVM(), &vm, Prepare}, {GetLaunchId, Subscribe, Disconnect},
      {GetWebViewOpenId, GetWebViewHandleWindowCloseId, GetWebViewProtocolName,
       GetWebViewAvailabilityId, GetWebViewMessageId, InitializeWebViewProtocol,
       Subscribe, Disconnect, SetRequestHandler, ClearNativeRequestHandler,
       PublishRaw, BroadcastDataModelFocus, GetWebViewMutateId,
       GetWebViewCloseId, SignalWebViewJavascriptCallback,
       UpdateCookieSetHandler},
      BrowserServiceSymbols(),
      {Foreground, Start, Update, PauseGame, ResumeGame, Leave, PauseApp,
       DestroyApp, UpdateApp, StartApp, CallMessagesFromMainThread},
      JniFactory(&probe), {&probe, RegisterObserver, ClearObserver}, {},
      nullptr, {}, {&probe, ObservePresence});
  RobloxLuaAppExperienceReadiness readiness;
  readiness.principal = {GameSessionPrincipalKind::kAuthenticated, 4, "42",
                         "https://www.roblox.com"};
  readiness.surface = {7, 0x1234, 1280, 720};
  readiness.username = "typed-user";

  ASSERT_TRUE(composition.InitializePlatformProtocols().ok());
  ASSERT_TRUE(composition.OnLuaAppReady(readiness).ok());
  ASSERT_TRUE(composition.subscribed());
  EXPECT_EQ(probe.web_view_initializations, 1);
  EXPECT_EQ(probe.browser_bindings, 4);
  EXPECT_EQ(probe.browser_callbacks_created, 4);
  EXPECT_TRUE(composition.SurfaceCreated(8).ok());
  EXPECT_TRUE(composition.SurfaceChanged({8, 0x5678, 640, 480}).ok());
  EXPECT_TRUE(composition.SurfaceDestroyed(8).ok());
  EXPECT_TRUE(composition.SurfaceCreated(9).ok());
  EXPECT_TRUE(composition.SurfaceChanged({9, 0x6789, 640, 480}).ok());
  EXPECT_EQ(probe.app_updates, 0);
  EXPECT_EQ(probe.app_starts, 2);
  EXPECT_EQ(probe.app_pauses, 2);
  EXPECT_EQ(probe.app_start_surface_width, 640);
  EXPECT_EQ(probe.app_user_id, 42);
  EXPECT_EQ(probe.app_surface_lifecycle,
            (std::vector<std::string>{"pause-app", "background:ASMA.stop",
                                      "foreground:ASMA.start", "start-app",
                                      "pause-app", "background:ASMA.stop",
                                      "foreground:ASMA.start", "start-app"}));
  ASSERT_NE(probe.callback, nullptr);
  JNIEnv* env = vm.GetJNIEnv();
  jclass callback_class = env->GetObjectClass(probe.callback);
  jmethodID run =
      env->GetMethodID(callback_class, "run", "(Ljava/lang/String;)V");
  jstring request = env->NewStringUTF(
      R"({"placeId":17580461965,"gameJoinContext":"PlayButton"})");
  env->CallVoidMethod(probe.callback, run, request);

  EXPECT_EQ(probe.starts.load(), 0);
  DrainUntilPresented(&composition);
  EXPECT_EQ(probe.starts.load(), 1);
  EXPECT_EQ(probe.started_place, 17580461965);
  EXPECT_EQ(probe.join_request_type, 0);
  EXPECT_EQ(probe.surface_width, 640);
  EXPECT_TRUE(composition.Snapshot().game_presented);
  ASSERT_EQ(probe.presence_phases.size(), 2u);
  EXPECT_EQ(probe.presence_phases[0], RobloxExperiencePresencePhase::kJoining);
  EXPECT_EQ(probe.presence_phases[1], RobloxExperiencePresencePhase::kPlaying);
  EXPECT_EQ(probe.presence_places,
            (std::vector<int64_t>{17580461965, 17580461965}));

  // ASMA/V2 reports its already-completed native return through the exact
  // NativeGLJavaInterface.gameDidLeave() callback. The host consumes it
  // without replaying LeaveGame, retains subsequent LuaApp surface updates,
  // and accepts another launch in the same process.
  std::shared_ptr<void> lifecycle_context(&composition, [](void*) {});
  vm.SetRobloxExperienceLifecycleCallbacks(
      lifecycle_context,
      jnivm::RobloxExperienceLifecycleCallbacks{&NotifyCompositionReturn});
  jclass native_gl_class =
      env->FindClass("com/roblox/engine/jni/NativeGLJavaInterface");
  jmethodID game_did_leave =
      env->GetStaticMethodID(native_gl_class, "gameDidLeave", "()V");
  ASSERT_NE(game_did_leave, nullptr);
  env->CallStaticVoidMethod(native_gl_class, game_did_leave);
  ASSERT_TRUE(composition.DrainLaunchRequests().ok());
  EXPECT_EQ(probe.starts.load(), 1);
  EXPECT_EQ(composition.Snapshot().state, GameSessionState::kReturnedToLuaApp);
  EXPECT_TRUE(composition.Snapshot().returned_to_lua_app);
  ASSERT_EQ(probe.presence_phases.size(), 3u);
  EXPECT_EQ(probe.presence_phases.back(),
            RobloxExperiencePresencePhase::kBrowsing);
  EXPECT_EQ(probe.presence_places.back(), 0);
  const int updates_before_lua_app_resize = probe.updates;
  const int app_updates_before_lua_app_resize = probe.app_updates;
  const GameSessionUpdateResult lua_app_resize =
      composition.SurfaceChanged({10, 0x7777, 800, 600});
  EXPECT_TRUE(lua_app_resize.ok());
  EXPECT_EQ(lua_app_resize.state, GameSessionState::kCreated);
  EXPECT_EQ(probe.updates, updates_before_lua_app_resize);
  EXPECT_EQ(probe.app_updates, app_updates_before_lua_app_resize + 1);
  EXPECT_EQ(probe.app_surface_width, 800);

  jstring server_request = env->NewStringUTF(
      R"({"placeId":1962086868,"gameInstanceId":"97ea-49e4",)"
      R"("joinAttemptId":"second-launch","joinAttemptOrigin":"WebView"})");
  env->CallVoidMethod(probe.callback, run, server_request);
  for (int attempt = 0; attempt < 1000 && probe.starts.load() < 2; ++attempt) {
    ASSERT_TRUE(composition.DrainLaunchRequests().ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_EQ(probe.starts.load(), 2);
  DrainUntilPresented(&composition);
  EXPECT_EQ(probe.started_place, 1962086868);
  EXPECT_EQ(probe.join_request_type, 3);
  EXPECT_EQ(probe.leaves, 0);
  ASSERT_EQ(probe.presence_phases.size(), 5u);
  EXPECT_EQ(probe.presence_phases[3], RobloxExperiencePresencePhase::kJoining);
  EXPECT_EQ(probe.presence_phases[4], RobloxExperiencePresencePhase::kPlaying);
  EXPECT_EQ(probe.presence_places[3], 1962086868);
  EXPECT_EQ(probe.presence_places[4], 1962086868);
  EXPECT_TRUE(composition.LeaveGame().ok());
  EXPECT_EQ(probe.leaves, 1);
  EXPECT_EQ(probe.destroys, 0);
  vm.ClearRobloxExperienceLifecycleCallbacks();
  EXPECT_TRUE(composition.Shutdown().ok());
  EXPECT_EQ(probe.disconnects, 4);
  EXPECT_EQ(probe.browser_disconnects, 4);
  EXPECT_EQ(probe.browser_releases, 4);
  EXPECT_EQ(probe.browser_callbacks_cleared, 4);
  g_probe = nullptr;
}

TEST(RobloxExperienceCompositionTest,
     AcceptsMessagePublishedSynchronouslyBySubscribe) {
  jnivm::VM vm;
  Probe probe{&vm};
  probe.publish_during_subscribe = true;
  g_probe = &probe;
  for (const char* class_name : {
           "com/roblox/engine/jni/NativeGLInterface",
           "android/view/Surface",
           "com/roblox/engine/jni/model/PlatformParams",
           "android/app/Activity",
           "com/roblox/engine/jni/autovalue/AutoValue_StartGameParams",
           "com/roblox/engine/jni/autovalue/StartAppParams",
           "com/roblox/universalapp/messagebus/MessageBus",
           "com/roblox/universalapp/messagebus/Connection",
           "com/roblox/engine/jni/memstorage/MemStorage",
           "com/roblox/engine/jni/memstorage/Connection",
           "com/roblox/engine/jni/memstorage/Callback",
       }) {
    vm.RegisterClass(class_name);
  }
  RobloxExperienceComposition composition(
      {vm.GetJavaVM(), &vm, Prepare}, {GetLaunchId, Subscribe, Disconnect},
      {GetWebViewOpenId, GetWebViewHandleWindowCloseId, GetWebViewProtocolName,
       GetWebViewAvailabilityId, GetWebViewMessageId, InitializeWebViewProtocol,
       Subscribe, Disconnect, SetRequestHandler, ClearNativeRequestHandler,
       PublishRaw, BroadcastDataModelFocus, GetWebViewMutateId,
       GetWebViewCloseId, SignalWebViewJavascriptCallback,
       UpdateCookieSetHandler},
      BrowserServiceSymbols(),
      {Foreground, Start, Update, PauseGame, ResumeGame, Leave, PauseApp,
       DestroyApp, UpdateApp, StartApp, CallMessagesFromMainThread},
      JniFactory(&probe), {&probe, RegisterObserver, ClearObserver});
  RobloxLuaAppExperienceReadiness readiness;
  readiness.principal = {GameSessionPrincipalKind::kAuthenticated, 4, "42",
                         "https://www.roblox.com"};
  readiness.surface = {7, 0x1234, 1280, 720};
  readiness.username = "typed-user";

  ASSERT_TRUE(composition.InitializePlatformProtocols().ok());
  ASSERT_TRUE(composition.OnLuaAppReady(readiness).ok());
  EXPECT_EQ(probe.starts.load(), 0);
  DrainUntilPresented(&composition);
  EXPECT_EQ(probe.starts.load(), 1);
  EXPECT_EQ(probe.started_place, 17580461965);
  EXPECT_TRUE(composition.Shutdown().ok());
  g_probe = nullptr;
}

TEST(RobloxExperienceCompositionTest,
     ConsumesControlledStartAndSwitchesActiveGame) {
  char directory_pattern[] = "/tmp/mocktail-composition-launch-test-XXXXXX";
  char* launch_directory = mkdtemp(directory_pattern);
  ASSERT_NE(launch_directory, nullptr);
  ASSERT_EQ(chmod(launch_directory, 0700), 0);
  ExternalLaunchBrokerOptions broker_options;
  broker_options.socket_path =
      std::filesystem::path(launch_directory) / "broker.sock";
  std::shared_ptr<ExternalLaunchBroker> broker;
  Status status = ExternalLaunchBroker::StartOwnerAfterLockAcquired(
      broker_options, &broker);
  ASSERT_TRUE(status.ok()) << status.message();
  ASSERT_TRUE(InstallActiveExternalLaunchBroker(broker).ok());
  RobloxExperienceLaunchRequest controlled_launch;
  ASSERT_TRUE(ParseRobloxExperienceLaunchJson(
                  R"({"placeId":1962086868,"gameInstanceId":"97ea-49e4",)"
                  R"("joinAttemptOrigin":"Web"})",
                  &controlled_launch)
                  .ok());
  ASSERT_TRUE(broker->QueueInitialRequest(std::move(controlled_launch)).ok());

  jnivm::VM vm;
  Probe probe{&vm};
  g_probe = &probe;
  for (const char* class_name : {
           "com/roblox/engine/jni/NativeGLInterface",
           "android/view/Surface",
           "com/roblox/engine/jni/model/PlatformParams",
           "android/app/Activity",
           "com/roblox/engine/jni/autovalue/AutoValue_StartGameParams",
           "com/roblox/engine/jni/autovalue/StartAppParams",
           "com/roblox/universalapp/messagebus/MessageBus",
           "com/roblox/universalapp/messagebus/Connection",
           "com/roblox/engine/jni/memstorage/MemStorage",
           "com/roblox/engine/jni/memstorage/Connection",
           "com/roblox/engine/jni/memstorage/Callback",
       }) {
    vm.RegisterClass(class_name);
  }
  RobloxExperienceComposition composition(
      {vm.GetJavaVM(), &vm, Prepare}, {GetLaunchId, Subscribe, Disconnect},
      {GetWebViewOpenId, GetWebViewHandleWindowCloseId, GetWebViewProtocolName,
       GetWebViewAvailabilityId, GetWebViewMessageId, InitializeWebViewProtocol,
       Subscribe, Disconnect, SetRequestHandler, ClearNativeRequestHandler,
       PublishRaw, BroadcastDataModelFocus, GetWebViewMutateId,
       GetWebViewCloseId, SignalWebViewJavascriptCallback,
       UpdateCookieSetHandler},
      BrowserServiceSymbols(),
      {Foreground, Start, Update, PauseGame, ResumeGame, Leave, PauseApp,
       DestroyApp, UpdateApp, StartApp, CallMessagesFromMainThread},
      JniFactory(&probe), {&probe, RegisterObserver, ClearObserver});
  RobloxLuaAppExperienceReadiness readiness;
  readiness.principal = {GameSessionPrincipalKind::kAuthenticated, 4, "42",
                         "https://www.roblox.com"};
  readiness.surface = {7, 0x1234, 1280, 720};
  readiness.username = "typed-user";

  ASSERT_TRUE(composition.InitializePlatformProtocols().ok());
  ASSERT_TRUE(composition.OnLuaAppReady(readiness).ok());
  EXPECT_EQ(broker->pending_launch_count(), 0u);
  EXPECT_EQ(probe.starts.load(), 0);
  DrainUntilPresented(&composition);
  EXPECT_EQ(probe.starts.load(), 1);
  EXPECT_EQ(probe.started_place, 1962086868);
  EXPECT_EQ(probe.join_request_type, 3);

  RobloxExperienceLaunchRequest duplicate_server;
  ASSERT_TRUE(ParseRobloxExperienceLaunchJson(
                  R"({"gameInstanceId":"97ea-49e4","placeId":1962086868,)"
                  R"("joinAttemptOrigin":"Web"})",
                  &duplicate_server)
                  .ok());
  ASSERT_TRUE(broker->QueueInitialRequest(std::move(duplicate_server)).ok());
  ASSERT_TRUE(composition.DrainLaunchRequests().ok());
  EXPECT_EQ(broker->pending_launch_count(), 0u);
  EXPECT_EQ(probe.leaves, 0);
  EXPECT_EQ(probe.starts.load(), 1);

  RobloxExperienceLaunchRequest next_server;
  ASSERT_TRUE(ParseRobloxExperienceLaunchJson(
                  R"({"placeId":17580461965,"gameInstanceId":"next-server"})",
                  &next_server)
                  .ok());
  ASSERT_TRUE(broker->QueueInitialRequest(std::move(next_server)).ok());
  ASSERT_TRUE(composition.DrainLaunchRequests().ok());
  EXPECT_EQ(probe.leaves, 1);
  EXPECT_EQ(probe.starts.load(), 1);
  ASSERT_TRUE(composition.DrainLaunchRequests().ok());
  EXPECT_EQ(probe.leaves, 1);
  EXPECT_EQ(probe.starts.load(), 1);

  // The exact gameDidLeave callback is the safe boundary between the old
  // controller and the replacement server launch.
  composition.NotifyLuaAppDidReturn();
  for (int attempt = 0; attempt < 1000 && probe.starts.load() < 2; ++attempt) {
    ASSERT_TRUE(composition.DrainLaunchRequests().ok());
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_EQ(probe.starts.load(), 2);
  EXPECT_EQ(probe.started_place, 17580461965);
  EXPECT_EQ(probe.join_request_type, 3);

  EXPECT_TRUE(composition.LeaveGame().ok());
  EXPECT_EQ(probe.leaves, 2);
  EXPECT_TRUE(composition.Shutdown().ok());
  ClearActiveExternalLaunchBroker(broker.get());
  EXPECT_TRUE(broker->Shutdown().ok());
  broker.reset();
  std::error_code ignored;
  std::filesystem::remove_all(launch_directory, ignored);
  g_probe = nullptr;
}

TEST(RobloxExperienceCompositionTest,
     DrainStartsOwnedLaunchWorkerWithoutBlockingOwnerThread) {
  jnivm::VM vm;
  Probe probe{&vm};
  probe.publish_during_subscribe = true;
  probe.block_start.store(true);
  g_probe = &probe;
  for (const char* class_name : {
           "com/roblox/engine/jni/NativeGLInterface",
           "android/view/Surface",
           "com/roblox/engine/jni/model/PlatformParams",
           "android/app/Activity",
           "com/roblox/engine/jni/autovalue/AutoValue_StartGameParams",
           "com/roblox/engine/jni/autovalue/StartAppParams",
           "com/roblox/universalapp/messagebus/MessageBus",
           "com/roblox/universalapp/messagebus/Connection",
           "com/roblox/engine/jni/memstorage/MemStorage",
           "com/roblox/engine/jni/memstorage/Connection",
           "com/roblox/engine/jni/memstorage/Callback",
       }) {
    vm.RegisterClass(class_name);
  }
  RobloxExperienceComposition composition(
      {vm.GetJavaVM(), &vm, Prepare}, {GetLaunchId, Subscribe, Disconnect},
      {GetWebViewOpenId, GetWebViewHandleWindowCloseId, GetWebViewProtocolName,
       GetWebViewAvailabilityId, GetWebViewMessageId, InitializeWebViewProtocol,
       Subscribe, Disconnect, SetRequestHandler, ClearNativeRequestHandler,
       PublishRaw, BroadcastDataModelFocus, GetWebViewMutateId,
       GetWebViewCloseId, SignalWebViewJavascriptCallback,
       UpdateCookieSetHandler},
      BrowserServiceSymbols(),
      {Foreground, Start, Update, PauseGame, ResumeGame, Leave, PauseApp,
       DestroyApp, UpdateApp, StartApp, CallMessagesFromMainThread},
      JniFactory(&probe), {&probe, RegisterObserver, ClearObserver}, {},
      nullptr, {}, {&probe, ObservePresence});
  RobloxLuaAppExperienceReadiness readiness;
  readiness.principal = {GameSessionPrincipalKind::kAuthenticated, 4, "42",
                         "https://www.roblox.com"};
  readiness.surface = {7, 0x1234, 1280, 720};
  readiness.username = "typed-user";

  ASSERT_TRUE(composition.InitializePlatformProtocols().ok());
  ASSERT_TRUE(composition.OnLuaAppReady(readiness).ok());
  const auto started_at = std::chrono::steady_clock::now();
  ASSERT_TRUE(composition.DrainLaunchRequests().ok());
  const auto elapsed = std::chrono::steady_clock::now() - started_at;
  EXPECT_LT(elapsed, std::chrono::milliseconds(100));
  for (int attempt = 0; attempt < 1000 && !probe.start_entered.load();
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(probe.start_entered.load());
  EXPECT_EQ(probe.starts.load(), 1);
  for (int attempt = 0;
       attempt < 1000 && probe.playing_presence_notifications.load() == 0;
       ++attempt) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const int playing_before_start_return =
      probe.playing_presence_notifications.load();
  probe.release_start.store(true);
  DrainUntilPresented(&composition);
  ASSERT_EQ(playing_before_start_return, 1);
  ASSERT_GE(probe.presence_phases.size(), 2u);
  EXPECT_EQ(probe.presence_phases.front(),
            RobloxExperiencePresencePhase::kJoining);
  EXPECT_EQ(probe.presence_phases.back(),
            RobloxExperiencePresencePhase::kPlaying);
  EXPECT_TRUE(composition.Shutdown().ok());
  g_probe = nullptr;
}

TEST(RobloxExperienceCompositionTest,
     RejectsIncompleteReadinessBeforeSubscribe) {
  jnivm::VM vm;
  Probe probe{&vm};
  RobloxExperienceComposition composition({vm.GetJavaVM(), &vm, Prepare}, {},
                                          {}, {}, {}, JniFactory(&probe), {});
  EXPECT_FALSE(
      composition.OnLuaAppReady(RobloxLuaAppExperienceReadiness{}).ok());
  EXPECT_FALSE(composition.subscribed());
}

TEST(RobloxExperienceCompositionTest,
     InitializesDurablePlatformProtocolsBeforeNativeBootstrap) {
  jnivm::VM vm;
  Probe probe{&vm};
  g_probe = &probe;
  for (const char* class_name : {
           "com/roblox/protocols/webview/WebViewProtocol",
           "com/roblox/universalapp/messagebus/MessageBus",
           "com/roblox/universalapp/messagebus/Connection",
           "com/roblox/engine/jni/memstorage/MemStorage",
           "com/roblox/engine/jni/memstorage/Connection",
           "com/roblox/engine/jni/memstorage/Callback",
       }) {
    vm.RegisterClass(class_name);
  }
  RobloxExperienceComposition composition(
      {vm.GetJavaVM(), &vm, Prepare}, {},
      {GetWebViewOpenId, GetWebViewHandleWindowCloseId, GetWebViewProtocolName,
       GetWebViewAvailabilityId, GetWebViewMessageId, InitializeWebViewProtocol,
       Subscribe, Disconnect, SetRequestHandler, ClearNativeRequestHandler,
       PublishRaw, BroadcastDataModelFocus, GetWebViewMutateId,
       GetWebViewCloseId, SignalWebViewJavascriptCallback,
       UpdateCookieSetHandler},
      BrowserServiceSymbols(), {}, JniFactory(&probe), {});

  ASSERT_TRUE(composition.InitializePlatformProtocols().ok());
  EXPECT_FALSE(composition.subscribed());
  EXPECT_EQ(probe.web_view_initializations, 1);
  EXPECT_EQ(probe.browser_bindings, 4);
  EXPECT_EQ(probe.browser_callbacks_created, 4);
  EXPECT_EQ(probe.browser_disconnects, 0);
  EXPECT_EQ(probe.browser_releases, 0);
  EXPECT_EQ(probe.browser_callbacks_cleared, 0);
  EXPECT_TRUE(composition.Shutdown().ok());
  EXPECT_EQ(probe.disconnects, 3);
  EXPECT_EQ(probe.browser_disconnects, 4);
  EXPECT_EQ(probe.browser_releases, 4);
  EXPECT_EQ(probe.browser_callbacks_cleared, 4);
  g_probe = nullptr;
}

TEST(RobloxExperienceCompositionTest,
     NativeSignInSubscribesAndLaunchesWithoutRestart) {
  jnivm::VM vm;
  Probe probe{&vm};
  probe.observed_surface = {11, 0x2468, 1280, 720};
  g_probe = &probe;
  for (const char* class_name : {
           "com/roblox/engine/jni/NativeGLInterface",
           "android/view/Surface",
           "com/roblox/engine/jni/model/PlatformParams",
           "android/app/Activity",
           "com/roblox/engine/jni/autovalue/AutoValue_StartGameParams",
           "com/roblox/engine/jni/autovalue/StartAppParams",
           "com/roblox/universalapp/messagebus/MessageBus",
           "com/roblox/universalapp/messagebus/Connection",
           "com/roblox/engine/jni/memstorage/MemStorage",
           "com/roblox/engine/jni/memstorage/Connection",
           "com/roblox/engine/jni/memstorage/Callback",
       }) {
    vm.RegisterClass(class_name);
  }
  RobloxExperienceComposition composition(
      {vm.GetJavaVM(), &vm, Prepare}, {GetLaunchId, Subscribe, Disconnect},
      {GetWebViewOpenId, GetWebViewHandleWindowCloseId, GetWebViewProtocolName,
       GetWebViewAvailabilityId, GetWebViewMessageId, InitializeWebViewProtocol,
       Subscribe, Disconnect, SetRequestHandler, ClearNativeRequestHandler,
       PublishRaw, BroadcastDataModelFocus, GetWebViewMutateId,
       GetWebViewCloseId, SignalWebViewJavascriptCallback,
       UpdateCookieSetHandler},
      BrowserServiceSymbols(),
      {Foreground, Start, Update, PauseGame, ResumeGame, Leave, PauseApp,
       DestroyApp, UpdateApp, StartApp, CallMessagesFromMainThread},
      JniFactory(&probe), {&probe, RegisterObserver, ClearObserver}, {},
      nullptr, {&probe, SnapshotSurface});

  ASSERT_TRUE(composition.InitializePlatformProtocols().ok());
  EXPECT_FALSE(composition.subscribed());
  jnivm::RobloxAuthIdentity identity;
  identity.user_id = 4242;
  identity.username = "live-user";
  identity.display_name = "Live User";
  const std::string credential = ".ROBLOSECURITY=_|live-test-credential";
  vm.SetRobloxCredentialProvider(&credential, &ProvideCredential);
  vm.SetRobloxAuthIdentity(identity);

  ASSERT_TRUE(composition.DrainPlatformEvents().ok());
  ASSERT_TRUE(composition.subscribed());
  ASSERT_NE(probe.callback, nullptr);

  probe.observed_surface.width = 960;
  ASSERT_TRUE(composition.DrainPlatformEvents().ok());
  EXPECT_EQ(probe.app_updates, 1);
  EXPECT_EQ(probe.app_surface_width, 960);

  JNIEnv* env = vm.GetJNIEnv();
  jclass callback_class = env->GetObjectClass(probe.callback);
  jmethodID run =
      env->GetMethodID(callback_class, "run", "(Ljava/lang/String;)V");
  env->CallVoidMethod(
      probe.callback, run,
      env->NewStringUTF(
          R"({"placeId":17580461965,"gameJoinContext":"PlayButton"})"));
  DrainUntilPresented(&composition);
  EXPECT_EQ(probe.starts.load(), 1);
  EXPECT_EQ(probe.started_place, 17580461965);
  EXPECT_EQ(probe.surface_width, 960);
  EXPECT_TRUE(vm.DispatchRobloxExperienceLuaAppDidReturn());
  EXPECT_TRUE(composition.DrainLaunchRequests().ok());

  EXPECT_TRUE(composition.Shutdown().ok());
  EXPECT_FALSE(vm.DispatchRobloxExperienceLuaAppDidReturn());
  g_probe = nullptr;
}

TEST(RobloxExperienceCompositionTest,
     RequiresPlatformProtocolsBeforeLuaAppReadiness) {
  jnivm::VM vm;
  Probe probe{&vm};
  RobloxExperienceComposition composition(
      {vm.GetJavaVM(), &vm, Prepare}, {GetLaunchId, Subscribe, Disconnect},
      {GetWebViewOpenId, GetWebViewHandleWindowCloseId, GetWebViewProtocolName,
       GetWebViewAvailabilityId, GetWebViewMessageId, InitializeWebViewProtocol,
       Subscribe, Disconnect, SetRequestHandler, ClearNativeRequestHandler,
       PublishRaw, BroadcastDataModelFocus, GetWebViewMutateId,
       GetWebViewCloseId, SignalWebViewJavascriptCallback,
       UpdateCookieSetHandler},
      BrowserServiceSymbols(),
      {Foreground, Start, Update, PauseGame, ResumeGame, Leave, PauseApp,
       DestroyApp, UpdateApp, StartApp, CallMessagesFromMainThread},
      JniFactory(&probe), {&probe, RegisterObserver, ClearObserver});
  RobloxLuaAppExperienceReadiness readiness;
  readiness.principal = {GameSessionPrincipalKind::kAuthenticated, 4, "42",
                         "https://www.roblox.com"};
  readiness.surface = {7, 0x1234, 1280, 720};
  readiness.username = "typed-user";

  const Status status = composition.OnLuaAppReady(readiness);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.code(), StatusCode::kFailedPrecondition);
  EXPECT_FALSE(composition.subscribed());
}

}  // namespace

class RobloxExperienceCompositionWebSurfaceTest : public ::testing::Test {
 protected:
  struct ExitProbe {
    int calls = 0;
  };

  static void ObserveExit(void* context) {
    ++static_cast<ExitProbe*>(context)->calls;
  }

  static std::unique_ptr<RobloxExperienceComposition> MakeComposition(
      const SecureRobloxCredential* credential = nullptr) {
    return std::make_unique<RobloxExperienceComposition>(
        JniEnvironmentProvider{}, RobloxExperienceMessageBusSymbols{},
        RobloxWebViewMessageBusSymbols{}, RobloxBrowserServiceSymbols{},
        RobloxGameSessionSymbols{}, RobloxExperienceJniFactory{},
        RobloxFreshLaunchPresentBoundary{}, RobloxGameSurfaceJniConfig{},
        credential);
  }

  static bool CookieSynchronized(
      const RobloxExperienceComposition* composition) {
    return composition->web_view_cookie_synchronized_;
  }

  static std::string CookieValue(
      const RobloxExperienceComposition* composition) {
    return std::string(composition->web_view_cookie_.value());
  }

  static void Activate(RobloxExperienceComposition* composition,
                       bool browser_service, uint64_t process_generation,
                       uint64_t logical_generation,
                       const std::shared_ptr<ExitProbe>& probe) {
    std::lock_guard<std::mutex> lock(composition->mutex_);
    composition->web_surface_route_ =
        browser_service
            ? RobloxExperienceComposition::WebSurfaceRoute::kBrowserService
            : RobloxExperienceComposition::WebSurfaceRoute::kWebView;
    composition->web_surface_process_generation_ = process_generation;
    composition->web_surface_logical_generation_ = logical_generation;
    composition->web_surface_logical_exit_observer_ = {
        probe, &RobloxExperienceCompositionWebSurfaceTest::ObserveExit};
  }

  static Status Close(RobloxExperienceComposition* composition) {
    return composition->CloseWebSurface();
  }

  static Status RouteEvent(RobloxExperienceComposition* composition,
                           bool browser_service,
                           const WebViewHelperEvent& event) {
    return RobloxExperienceComposition::RouteWebSurfaceEvent(
        browser_service
            ? RobloxExperienceComposition::WebSurfaceRoute::kBrowserService
            : RobloxExperienceComposition::WebSurfaceRoute::kWebView,
        event, composition->web_view_bridge_.get());
  }

  static Status RouteCurrentEvent(RobloxExperienceComposition* composition,
                                  uint64_t process_generation,
                                  uint64_t logical_generation,
                                  const WebViewHelperEvent& event) {
    return composition->RouteCurrentWebSurfaceEvent(nullptr, process_generation,
                                                    logical_generation, event);
  }

  static void PhysicalExit(RobloxExperienceComposition* composition,
                           uint64_t process_generation) {
    composition->HandleWebSurfaceExit(process_generation);
  }
};

TEST_F(RobloxExperienceCompositionWebSurfaceTest,
       SeedsValidatedWebViewCredentialBeforePlatformCallbacks) {
  SecureRobloxCredential credential{".ROBLOSECURITY=typed-secret"};
  auto composition = MakeComposition(&credential);

  EXPECT_TRUE(CookieSynchronized(composition.get()));
  EXPECT_EQ(CookieValue(composition.get()), "typed-secret");
}

TEST_F(RobloxExperienceCompositionWebSurfaceTest,
       RoutesServerJoinToExactApkCallbackOwnerWithoutRewritingPayload) {
  jnivm::VM vm;
  Probe probe{&vm};
  g_probe = &probe;
  for (const char* class_name : {
           "com/roblox/protocols/webview/WebViewProtocol",
           "com/roblox/universalapp/messagebus/MessageBus",
           "com/roblox/universalapp/messagebus/Connection",
           "com/roblox/engine/jni/memstorage/MemStorage",
           "com/roblox/engine/jni/memstorage/Connection",
           "com/roblox/engine/jni/memstorage/Callback",
       }) {
    vm.RegisterClass(class_name);
  }
  RobloxExperienceComposition composition(
      {vm.GetJavaVM(), &vm, Prepare}, {},
      {GetWebViewOpenId, GetWebViewHandleWindowCloseId, GetWebViewProtocolName,
       GetWebViewAvailabilityId, GetWebViewMessageId, InitializeWebViewProtocol,
       Subscribe, Disconnect, SetRequestHandler, ClearNativeRequestHandler,
       PublishRaw, BroadcastDataModelFocus, GetWebViewMutateId,
       GetWebViewCloseId, SignalWebViewJavascriptCallback,
       UpdateCookieSetHandler},
      BrowserServiceSymbols(), {}, JniFactory(&probe), {});
  ASSERT_TRUE(composition.InitializePlatformProtocols().ok());

  const std::string join =
      R"({"moduleID":"Game","functionName":"launchGame",)"
      R"("params":{"request":{"requestType":"RequestGameJob",)"
      R"("placeId":"1962086868","instanceId":"97ea-49e4",)"
      R"("isPlayTogetherGame":false,"browserTrackerId":"tracker"}},)"
      R"("callbackID":"join-callback"})";
  ASSERT_TRUE(RouteEvent(&composition, false,
                         {WebViewHelperEventType::kExecuteRoblox, join})
                  .ok());
  EXPECT_EQ(probe.web_view_javascript_callback, join);

  probe.web_view_javascript_callback.clear();
  ASSERT_TRUE(RouteEvent(&composition, true,
                         {WebViewHelperEventType::kExecuteRoblox, join})
                  .ok());
  EXPECT_EQ(probe.web_view_javascript_callback, join);

  auto exit_probe = std::make_shared<ExitProbe>();
  Activate(&composition, false, 17, 23, exit_probe);
  probe.web_view_javascript_callback.clear();
  ASSERT_TRUE(RouteCurrentEvent(&composition, 17, 23,
                                {WebViewHelperEventType::kExecuteRoblox, join})
                  .ok());
  EXPECT_EQ(probe.web_view_javascript_callback, join);

  ASSERT_TRUE(Close(&composition).ok());
  probe.web_view_javascript_callback.clear();
  EXPECT_TRUE(RouteCurrentEvent(&composition, 17, 23,
                                {WebViewHelperEventType::kExecuteRoblox, join})
                  .ok());
  EXPECT_TRUE(probe.web_view_javascript_callback.empty());

  Activate(&composition, false, 17, 24, exit_probe);
  EXPECT_TRUE(RouteCurrentEvent(&composition, 17, 23,
                                {WebViewHelperEventType::kExecuteRoblox, join})
                  .ok());
  EXPECT_TRUE(probe.web_view_javascript_callback.empty());
  ASSERT_TRUE(RouteCurrentEvent(&composition, 17, 24,
                                {WebViewHelperEventType::kExecuteRoblox, join})
                  .ok());
  EXPECT_EQ(probe.web_view_javascript_callback, join);
  EXPECT_TRUE(Close(&composition).ok());

  EXPECT_TRUE(composition.Shutdown().ok());
  g_probe = nullptr;
}

TEST_F(RobloxExperienceCompositionWebSurfaceTest,
       LogicalCloseNotifiesActiveRouteExactlyOnce) {
  auto composition = MakeComposition();
  auto probe = std::make_shared<ExitProbe>();
  Activate(composition.get(), false, 7, 11, probe);

  EXPECT_TRUE(Close(composition.get()).ok());
  EXPECT_TRUE(Close(composition.get()).ok());
  EXPECT_EQ(probe->calls, 1);
}

TEST_F(RobloxExperienceCompositionWebSurfaceTest,
       CrossProtocolCloseOwnsSharedSurfaceAndIgnoresStaleGeneration) {
  auto composition = MakeComposition();
  auto probe = std::make_shared<ExitProbe>();
  Activate(composition.get(), true, 9, 13, probe);

  EXPECT_TRUE(Close(composition.get()).ok());
  EXPECT_EQ(probe->calls, 1);
  PhysicalExit(composition.get(), 8);
  EXPECT_EQ(probe->calls, 1);

  PhysicalExit(composition.get(), 9);
  PhysicalExit(composition.get(), 9);
  EXPECT_EQ(probe->calls, 1);
}

namespace {}  // namespace
}  // namespace runtime
}  // namespace mocktail
