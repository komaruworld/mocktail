#include "runtime/roblox_fresh_game_launch_controller.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "jnivm/jnivm.h"

namespace mocktail {
namespace runtime {
namespace {

struct Probe {
  JNIEnv* env = nullptr;
  std::vector<std::string> calls;
  std::vector<int64_t> places;
  int64_t user_id = 0;
  int64_t conversation_id = 0;
  int64_t referred_by_player_id = 0;
  int32_t join_request_type = 0;
  bool under_13 = false;
  std::string game_id;
  std::string access_code;
  std::string launch_data;
  std::string username;
  jobject surface = nullptr;
  jobject platform_params = nullptr;
  jobject device_params = nullptr;
  jobject vr_context = nullptr;
  FreshLaunchPresentObserver observer = nullptr;
  void* observer_context = nullptr;
  bool register_result = true;
  bool present_during_start = false;
  bool cleared = false;
};

Probe* g_probe = nullptr;

std::string StringField(JNIEnv* env, jobject object, jclass clazz,
                        const char* name) {
  jmethodID method = env->GetMethodID(clazz, name, "()Ljava/lang/String;");
  auto value = static_cast<jstring>(env->CallObjectMethod(object, method));
  const char* chars = env->GetStringUTFChars(value, nullptr);
  std::string result = chars != nullptr ? chars : "";
  env->ReleaseStringUTFChars(value, chars);
  return result;
}

void SetBackground(JNIEnv*, jclass, jboolean enabled, jstring) {
  g_probe->calls.emplace_back(enabled == JNI_TRUE ? "background"
                                                  : "foreground");
}

jint StartGame(JNIEnv* env, jclass, jobject params) {
  g_probe->calls.emplace_back("start");
  jclass clazz = env->GetObjectClass(params);
  g_probe->places.push_back(
      env->CallLongMethod(params, env->GetMethodID(clazz, "placeId", "()J")));
  g_probe->user_id =
      env->CallLongMethod(params, env->GetMethodID(clazz, "userId", "()J"));
  g_probe->conversation_id = env->CallLongMethod(
      params, env->GetMethodID(clazz, "conversationId", "()J"));
  g_probe->referred_by_player_id = env->CallLongMethod(
      params, env->GetMethodID(clazz, "referredByPlayerId", "()J"));
  g_probe->join_request_type = env->CallIntMethod(
      params, env->GetMethodID(clazz, "joinRequestType", "()I"));
  g_probe->under_13 =
      env->CallBooleanMethod(
          params, env->GetMethodID(clazz, "isUnder13", "()Z")) == JNI_TRUE;
  g_probe->game_id = StringField(env, params, clazz, "gameId");
  g_probe->access_code = StringField(env, params, clazz, "accessCode");
  g_probe->launch_data = StringField(env, params, clazz, "launchData");
  g_probe->username = StringField(env, params, clazz, "username");
  g_probe->surface = env->CallObjectMethod(
      params, env->GetMethodID(clazz, "surface", "()Landroid/view/Surface;"));
  g_probe->platform_params = env->CallObjectMethod(
      params,
      env->GetMethodID(clazz, "platformParams",
                       "()Lcom/roblox/engine/jni/model/PlatformParams;"));
  g_probe->device_params = env->CallObjectMethod(
      params, env->GetMethodID(clazz, "deviceParams",
                               "()Lcom/roblox/engine/jni/model/DeviceParams;"));
  g_probe->vr_context = env->CallObjectMethod(
      params, env->GetMethodID(clazz, "vrContext", "()Landroid/app/Activity;"));
  if (g_probe->present_during_start && g_probe->observer != nullptr) {
    g_probe->observer(g_probe->observer_context, 77);
  }
  return 1;
}

void UpdateSurface(JNIEnv*, jclass, jobject, jobject, jobject) {
  g_probe->calls.emplace_back("update-surface");
}
void PauseGame(JNIEnv*, jclass) { g_probe->calls.emplace_back("pause-game"); }
void ResumeGame(JNIEnv*, jclass, jobject, jobject, jobject) {
  g_probe->calls.emplace_back("resume-game");
}
void LeaveGame(JNIEnv*, jclass) { g_probe->calls.emplace_back("leave"); }
void PauseApp(JNIEnv*, jclass) { g_probe->calls.emplace_back("pause-app"); }
void DestroyApp(JNIEnv*, jclass) { g_probe->calls.emplace_back("destroy-app"); }
void UpdateAppSurface(JNIEnv*, jclass, jobject, jobject) {}
void StartApp(JNIEnv*, jclass, jobject) {}

void CallMessagesFromMainThread(JNIEnv*, jclass) {}

RobloxGameSessionSymbols Symbols() {
  return {SetBackground, StartGame, UpdateSurface, PauseGame,        ResumeGame,
          LeaveGame,     PauseApp,  DestroyApp,    UpdateAppSurface, StartApp,
          CallMessagesFromMainThread};
}

bool RegisterObserver(void* context, FreshLaunchPresentObserver observer,
                      void* observer_context) {
  auto* probe = static_cast<Probe*>(context);
  probe->calls.emplace_back("register-observer");
  if (!probe->register_result) {
    return false;
  }
  probe->observer = observer;
  probe->observer_context = observer_context;
  return true;
}

void ClearObserver(void* context) {
  auto* probe = static_cast<Probe*>(context);
  probe->calls.emplace_back("clear-observer");
  probe->observer = nullptr;
  probe->observer_context = nullptr;
  probe->cleared = true;
}

void Prepare(void* context) {
  static_cast<jnivm::VM*>(context)->RestoreFunctions();
}

class RobloxFreshGameLaunchControllerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    g_probe = &probe_;
    vm_ = std::make_unique<jnivm::VM>();
    for (const char* name : {
             "com/roblox/engine/jni/NativeGLInterface",
             "android/view/Surface",
             "com/roblox/engine/jni/model/PlatformParams",
             "com/roblox/engine/jni/model/DeviceParams",
             "android/app/Activity",
             "com/roblox/engine/jni/autovalue/StartGameParams",
             "com/roblox/engine/jni/autovalue/AutoValue_StartGameParams",
         }) {
      vm_->RegisterClass(name);
    }
    probe_.env = vm_->GetJNIEnv();
  }

  void TearDown() override {
    vm_->RestoreFunctions();
    g_probe = nullptr;
  }

  RobloxExperienceLaunchRequest Request(int64_t place_id = 123456) {
    RobloxExperienceLaunchRequest request;
    const std::string json =
        std::string("{\"placeId\":") + std::to_string(place_id) +
        R"(,"userId":42,"conversationId":7,"referredByPlayerId":9,)"
        R"("gameInstanceId":"instance","accessCode":"access",)"
        R"("launchData":"payload","reservedServerAccessCode":"reserved",)"
        R"("callId":"call","linkCode":"link","referralPage":"detail",)"
        R"("gameJoinContext":"PlayButton","eventId":"event",)"
        R"("joinAttemptId":"attempt","joinAttemptOrigin":"LuaApp",)"
        R"("isoContext":"iso"})";
    EXPECT_TRUE(ParseRobloxExperienceLaunchJson(json, &request).ok());
    return request;
  }

  RobloxFreshGameLaunchContext Context() {
    JNIEnv* env = vm_->GetJNIEnv();
    RobloxFreshGameLaunchContext context;
    context.request_id = 88;
    context.native_gl_class =
        env->FindClass("com/roblox/engine/jni/NativeGLInterface");
    context.surface = env->AllocObject(env->FindClass("android/view/Surface"));
    context.platform_params = env->AllocObject(
        env->FindClass("com/roblox/engine/jni/model/PlatformParams"));
    context.device_params = env->AllocObject(
        env->FindClass("com/roblox/engine/jni/model/DeviceParams"));
    context.activity = env->AllocObject(env->FindClass("android/app/Activity"));
    context.vr_context = context.activity;
    context.principal = {GameSessionPrincipalKind::kAuthenticated, 5,
                         "6018634220", "https://www.roblox.com"};
    context.game_surface = {12, 0x1234, 1280, 720};
    context.username = "typed-user";
    context.is_under_13 = true;
    context.join_request_type = 6;
    return context;
  }

  JniEnvironmentProvider Environment() {
    return {vm_->GetJavaVM(), vm_.get(), Prepare};
  }

  RobloxFreshLaunchPresentBoundary PresentBoundary() {
    return {&probe_, RegisterObserver, ClearObserver};
  }

  Probe probe_;
  std::unique_ptr<jnivm::VM> vm_;
};

TEST_F(RobloxFreshGameLaunchControllerTest,
       BuildsEveryLaunchFieldFromFreshTypedRequest) {
  RobloxFreshGameLaunchController controller(Environment(), Symbols(),
                                             PresentBoundary());
  const RobloxExperienceLaunchRequest request = Request();
  const RobloxFreshGameLaunchContext context = Context();

  const Status status = controller.Launch(request, context);

  ASSERT_TRUE(status.ok()) << status.message();
  ASSERT_EQ(probe_.places, (std::vector<int64_t>{123456}));
  EXPECT_EQ(probe_.user_id, 42);
  EXPECT_EQ(probe_.conversation_id, 7);
  EXPECT_EQ(probe_.referred_by_player_id, 9);
  EXPECT_EQ(probe_.join_request_type, 6);
  EXPECT_TRUE(probe_.under_13);
  EXPECT_EQ(probe_.game_id, "instance");
  EXPECT_EQ(probe_.access_code, "access");
  EXPECT_EQ(probe_.launch_data, "payload");
  EXPECT_EQ(probe_.username, "typed-user");
  EXPECT_EQ(probe_.surface, context.surface);
  EXPECT_EQ(probe_.platform_params, context.platform_params);
  EXPECT_EQ(probe_.device_params, context.device_params);
  EXPECT_EQ(probe_.vr_context, context.vr_context);
  EXPECT_EQ(probe_.calls[0], "register-observer");
  EXPECT_EQ(probe_.calls[1], "foreground");
  EXPECT_EQ(probe_.calls[2], "start");
}

TEST_F(RobloxFreshGameLaunchControllerTest,
       CapturesHostPresentEmittedDuringNativeStart) {
  probe_.present_during_start = true;
  RobloxFreshGameLaunchController controller(Environment(), Symbols(),
                                             PresentBoundary());
  ASSERT_TRUE(controller.Launch(Request(), Context()).ok());

  const GameSessionSnapshot snapshot = controller.Snapshot();
  EXPECT_TRUE(snapshot.game_presented);
  EXPECT_EQ(snapshot.first_presented_frame, 77u);
}

TEST_F(RobloxFreshGameLaunchControllerTest,
       ShutdownDrainsObserverBeforeDestroyingSessionRuntime) {
  RobloxFreshGameLaunchController controller(Environment(), Symbols(),
                                             PresentBoundary());
  ASSERT_TRUE(controller.Launch(Request(), Context()).ok());
  ASSERT_TRUE(controller.Shutdown().ok());

  const auto clear =
      std::find(probe_.calls.begin(), probe_.calls.end(), "clear-observer");
  const auto leave =
      std::find(probe_.calls.begin(), probe_.calls.end(), "leave");
  const auto destroy =
      std::find(probe_.calls.begin(), probe_.calls.end(), "destroy-app");
  ASSERT_NE(clear, probe_.calls.end());
  ASSERT_NE(leave, probe_.calls.end());
  ASSERT_NE(destroy, probe_.calls.end());
  EXPECT_LT(clear, leave);
  EXPECT_LT(leave, destroy);
  EXPECT_LT(clear, destroy);
  EXPECT_TRUE(probe_.cleared);
  EXPECT_FALSE(controller.active());
}

TEST_F(RobloxFreshGameLaunchControllerTest,
       SequentialSessionsNeverReuseStaleStartGameParams) {
  RobloxFreshGameLaunchController controller(Environment(), Symbols(),
                                             PresentBoundary());
  ASSERT_TRUE(controller.Launch(Request(111), Context()).ok());
  ASSERT_TRUE(controller.Leave().ok());
  EXPECT_TRUE(controller.Snapshot().returned_to_lua_app);
  RobloxFreshGameLaunchContext second_context = Context();
  second_context.request_id = 89;
  ASSERT_TRUE(controller.Launch(Request(222), second_context).ok());

  EXPECT_EQ(probe_.places, (std::vector<int64_t>{111, 222}));
  EXPECT_EQ(std::count(probe_.calls.begin(), probe_.calls.end(), "leave"), 1);
  EXPECT_EQ(std::count(probe_.calls.begin(), probe_.calls.end(), "destroy-app"),
            0);
  const auto leave =
      std::find(probe_.calls.begin(), probe_.calls.end(), "leave");
  const auto clear =
      std::find(probe_.calls.begin(), probe_.calls.end(), "clear-observer");
  ASSERT_NE(leave, probe_.calls.end());
  ASSERT_NE(clear, probe_.calls.end());
  EXPECT_LT(leave, clear);
}

TEST_F(RobloxFreshGameLaunchControllerTest,
       ObservedNativeLuaAppReturnReleasesSessionWithoutReplayingLifecycle) {
  RobloxFreshGameLaunchController controller(Environment(), Symbols(),
                                             PresentBoundary());
  ASSERT_TRUE(controller.Launch(Request(111), Context()).ok());

  ASSERT_TRUE(controller.ObserveLuaAppReturn().ok());
  EXPECT_FALSE(controller.active());
  EXPECT_TRUE(controller.Snapshot().returned_to_lua_app);
  EXPECT_EQ(controller.Snapshot().state, GameSessionState::kReturnedToLuaApp);
  EXPECT_EQ(std::count(probe_.calls.begin(), probe_.calls.end(), "leave"), 0);
  EXPECT_EQ(std::count(probe_.calls.begin(), probe_.calls.end(), "pause-app"),
            0);
  EXPECT_EQ(std::count(probe_.calls.begin(), probe_.calls.end(), "destroy-app"),
            0);
  EXPECT_EQ(
      std::count(probe_.calls.begin(), probe_.calls.end(), "clear-observer"),
      1);

  // Duplicate stage observations are nonfatal and must not clear twice.
  EXPECT_TRUE(controller.ObserveLuaAppReturn().ok());
  EXPECT_EQ(
      std::count(probe_.calls.begin(), probe_.calls.end(), "clear-observer"),
      1);

  RobloxFreshGameLaunchContext second_context = Context();
  second_context.request_id = 89;
  ASSERT_TRUE(controller.Launch(Request(222), second_context).ok());
  EXPECT_EQ(probe_.places, (std::vector<int64_t>{111, 222}));
  EXPECT_EQ(std::count(probe_.calls.begin(), probe_.calls.end(), "leave"), 0);
  EXPECT_EQ(std::count(probe_.calls.begin(), probe_.calls.end(), "pause-app"),
            0);
  EXPECT_EQ(std::count(probe_.calls.begin(), probe_.calls.end(), "destroy-app"),
            0);
}

TEST_F(RobloxFreshGameLaunchControllerTest,
       ForwardsRecreatedSurfaceLifecycleToActiveSession) {
  RobloxFreshGameLaunchController controller(Environment(), Symbols(),
                                             PresentBoundary());
  ASSERT_TRUE(controller.Launch(Request(), Context()).ok());

  EXPECT_TRUE(controller.SurfaceDestroyed(12).ok());
  EXPECT_TRUE(controller.SurfaceCreated(13).ok());
  EXPECT_TRUE(controller.SurfaceChanged({13, 0x5678, 1024, 768}).ok());

  const GameSessionSnapshot snapshot = controller.Snapshot();
  EXPECT_TRUE(snapshot.game_running);
  EXPECT_EQ(snapshot.active_surface_generation, 13u);
  EXPECT_NE(std::find(probe_.calls.begin(), probe_.calls.end(), "pause-game"),
            probe_.calls.end());
  EXPECT_NE(std::find(probe_.calls.begin(), probe_.calls.end(), "resume-game"),
            probe_.calls.end());
}

TEST_F(RobloxFreshGameLaunchControllerTest,
       RejectsSurfaceEventsWithoutAnActiveSession) {
  RobloxFreshGameLaunchController controller(Environment(), Symbols(),
                                             PresentBoundary());

  const GameSessionUpdateResult result =
      controller.SurfaceChanged({1, 0x1234, 1280, 720});

  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.cause.code(), StatusCode::kFailedPrecondition);
}

TEST_F(RobloxFreshGameLaunchControllerTest,
       AppShutdownIsTerminalForFutureLaunches) {
  RobloxFreshGameLaunchController controller(Environment(), Symbols(),
                                             PresentBoundary());
  ASSERT_TRUE(controller.Launch(Request(), Context()).ok());
  ASSERT_TRUE(controller.Shutdown().ok());

  const Status status = controller.Launch(Request(999), Context());

  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.code(), StatusCode::kFailedPrecondition);
  EXPECT_EQ(probe_.places, (std::vector<int64_t>{123456}));
}

TEST_F(RobloxFreshGameLaunchControllerTest,
       ObserverRegistrationFailurePreventsNativeStart) {
  probe_.register_result = false;
  RobloxFreshGameLaunchController controller(Environment(), Symbols(),
                                             PresentBoundary());

  const Status status = controller.Launch(Request(), Context());

  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.code(), StatusCode::kUnavailable);
  EXPECT_EQ(probe_.calls, (std::vector<std::string>{"register-observer"}));
  EXPECT_FALSE(controller.active());
}

TEST_F(RobloxFreshGameLaunchControllerTest,
       RejectsInvalidOrConcurrentLaunchWithoutReplacingActiveSession) {
  RobloxFreshGameLaunchController controller(Environment(), Symbols(),
                                             PresentBoundary());
  RobloxExperienceLaunchRequest invalid = Request();
  invalid.place_id = 0;
  EXPECT_FALSE(controller.Launch(invalid, Context()).ok());

  ASSERT_TRUE(controller.Launch(Request(), Context()).ok());
  const Status duplicate = controller.Launch(Request(999), Context());
  EXPECT_FALSE(duplicate.ok());
  EXPECT_EQ(duplicate.code(), StatusCode::kFailedPrecondition);
  EXPECT_EQ(probe_.places, (std::vector<int64_t>{123456}));
}

TEST_F(RobloxFreshGameLaunchControllerTest,
       RejectsTypedFieldsThatDoNotMatchCanonicalLaunchJson) {
  RobloxFreshGameLaunchController controller(Environment(), Symbols(),
                                             PresentBoundary());
  RobloxExperienceLaunchRequest inconsistent = Request();
  inconsistent.place_id = 999;

  const Status status = controller.Launch(inconsistent, Context());

  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.code(), StatusCode::kInvalidArgument);
  EXPECT_TRUE(probe_.places.empty());
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
