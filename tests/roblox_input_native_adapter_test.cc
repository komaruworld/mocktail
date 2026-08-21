#include "runtime/roblox_input_native_adapter.h"

#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "jnivm/jnivm.h"

namespace mocktail {
namespace runtime {

namespace {

struct NativeCall {
  std::string name;
  JNIEnv* env = nullptr;
  jclass clazz = nullptr;
  std::vector<double> arguments;
  std::u16string text;
};

struct Probe {
  std::vector<NativeCall> calls;
  int prepare_count = 0;
};

Probe* g_probe = nullptr;

void Record(const char* name, JNIEnv* env, jclass clazz,
            std::vector<double> arguments) {
  ASSERT_NE(g_probe, nullptr);
  g_probe->calls.push_back(
      {name != nullptr ? name : "", env, clazz, std::move(arguments), {}});
}

void PassMouseMove(JNIEnv* env, jclass clazz, jfloat x, jfloat y,
                   jfloat delta_x, jfloat delta_y) {
  Record("mouse-move", env, clazz, {x, y, delta_x, delta_y});
}

void PassMouseButton(JNIEnv* env, jclass clazz, jfloat x, jfloat y,
                     jboolean pressed, jint button) {
  Record("mouse-button", env, clazz,
         {x, y, static_cast<double>(pressed), static_cast<double>(button)});
}

void PassMouseWheel(JNIEnv* env, jclass clazz, jfloat x, jfloat y,
                    jfloat delta_y) {
  Record("mouse-wheel", env, clazz, {x, y, delta_y});
}

void PassInput(JNIEnv* env, jclass clazz, jint pointer_id, jfloat x, jfloat y,
               jint action, jint width, jint height) {
  Record("touch", env, clazz,
         {static_cast<double>(pointer_id), x, y, static_cast<double>(action),
          static_cast<double>(width), static_cast<double>(height)});
}

void PassKey(JNIEnv* env, jclass clazz, jboolean pressed, jint scan_code,
             jint key_code, jboolean repeat) {
  Record("key", env, clazz,
         {static_cast<double>(pressed), static_cast<double>(scan_code),
          static_cast<double>(key_code), static_cast<double>(repeat)});
}

jboolean GetMouseLockedCenter(JNIEnv* env, jclass clazz) {
  Record("mouse-lock-query", env, clazz, {});
  return JNI_TRUE;
}

jobject GetTextBoxInfo(JNIEnv* env, jclass clazz) {
  Record("text-box-info", env, clazz, {});
  jclass info_class =
      env->FindClass("com/roblox/engine/jni/model/NativeTextBoxInfo");
  if (info_class == nullptr) {
    return nullptr;
  }
  jmethodID constructor =
      env->GetMethodID(info_class, "<init>", "(FFFFFZIIIIIIZZ)V");
  if (constructor == nullptr) {
    env->DeleteLocalRef(info_class);
    return nullptr;
  }
  jvalue arguments[14]{};
  arguments[0].f = 69.0F;
  arguments[1].f = 36.0F;
  arguments[2].f = 633.0F;
  arguments[3].f = 36.0F;
  arguments[4].f = 16.0F;
  arguments[6].i = 2;
  arguments[7].i = 1;
  arguments[8].i = static_cast<jint>(0xffd4d8e1U);
  arguments[10].i = 3;
  jobject result = env->NewObjectA(info_class, constructor, arguments);
  env->DeleteLocalRef(info_class);
  return result;
}

std::u16string JavaText(JNIEnv* env, jstring text) {
  const jsize size = env->GetStringLength(text);
  const jchar* chars = env->GetStringChars(text, nullptr);
  std::u16string result(reinterpret_cast<const char16_t*>(chars),
                        static_cast<std::size_t>(size));
  env->ReleaseStringChars(text, chars);
  return result;
}

void SyncText(JNIEnv* env, jclass clazz, jstring text, jint cursor) {
  Record("sync-text", env, clazz, {static_cast<double>(cursor)});
  g_probe->calls.back().text = JavaText(env, text);
}

void PassText(JNIEnv* env, jclass clazz, jlong handle, jstring text,
              jboolean finished, jint cursor) {
  Record("pass-text", env, clazz,
         {static_cast<double>(handle), static_cast<double>(finished),
          static_cast<double>(cursor)});
  g_probe->calls.back().text = JavaText(env, text);
}

void ReturnPressed(JNIEnv* env, jclass clazz, jlong handle) {
  Record("return", env, clazz, {static_cast<double>(handle)});
}

void ReleaseFocus(JNIEnv* env, jclass clazz, jlong handle) {
  Record("release", env, clazz, {static_cast<double>(handle)});
}

void Prepare(void* context) {
  auto* pair = static_cast<std::pair<jnivm::VM*, Probe*>*>(context);
  ASSERT_NE(pair, nullptr);
  pair->first->RestoreFunctions();
  ++pair->second->prepare_count;
}

RobloxInputSymbols Symbols() {
  RobloxInputSymbols symbols;
  symbols.pass_mouse_move = PassMouseMove;
  symbols.pass_mouse_button = PassMouseButton;
  symbols.pass_mouse_wheel = PassMouseWheel;
  symbols.pass_input = PassInput;
  symbols.pass_key_event = PassKey;
  symbols.get_main_window_is_mouse_locked_center = GetMouseLockedCenter;
  symbols.sync_textbox_text_and_cursor_position2 = SyncText;
  symbols.get_text_box_info = GetTextBoxInfo;
  symbols.pass_text = PassText;
  symbols.return_pressed_from_on_screen_keyboard = ReturnPressed;
  symbols.release_focus = ReleaseFocus;
  return symbols;
}

platform::PlatformEvent Event(platform::PlatformEventPayload payload) {
  return {123, std::move(payload)};
}

class RobloxInputNativeAdapterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    g_probe = &probe_;
    vm_ = std::make_unique<jnivm::VM>();
    vm_->RegisterClass("com/roblox/engine/jni/NativeInputInterface");
    vm_->RegisterClass("com/roblox/engine/jni/NativeGLInterface");
    environment_context_ = {vm_.get(), &probe_};
  }

  void TearDown() override { g_probe = nullptr; }

  JniEnvironmentProvider Environment() {
    return {vm_->GetJavaVM(), &environment_context_, Prepare};
  }

  Probe probe_;
  std::unique_ptr<jnivm::VM> vm_;
  std::pair<jnivm::VM*, Probe*> environment_context_;
};

TEST_F(RobloxInputNativeAdapterTest, RejectsIncompleteInteractiveSymbolSet) {
  RobloxInputSymbols symbols = Symbols();
  symbols.pass_key_event = nullptr;
  RobloxInputNativeAdapter adapter(Environment(), symbols);

  Status status = adapter.Initialize();

  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.code(), StatusCode::kUnavailable);
  EXPECT_FALSE(adapter.initialized());
}

TEST_F(RobloxInputNativeAdapterTest,
       QueriesCurrentNativeTextBoxInfoThroughTypedExport) {
  RobloxInputNativeAdapter adapter(Environment(), Symbols());
  ASSERT_TRUE(adapter.Initialize().ok());
  RobloxNativeTextBoxInfoQueryResult result;

  const Status status = adapter.QueryCurrentTextBoxInfo(&result);

  ASSERT_TRUE(status.ok()) << status.message();
  ASSERT_TRUE(result.available);
  EXPECT_FLOAT_EQ(result.info.x, 69.0F);
  EXPECT_FLOAT_EQ(result.info.y, 36.0F);
  EXPECT_FLOAT_EQ(result.info.width, 633.0F);
  EXPECT_FLOAT_EQ(result.info.height, 36.0F);
  EXPECT_FLOAT_EQ(result.info.font_size, 16.0F);
  EXPECT_EQ(result.info.x_alignment, 2);
  EXPECT_EQ(result.info.text_input_type, 3);
  ASSERT_FALSE(probe_.calls.empty());
  EXPECT_EQ(probe_.calls.back().name, "text-box-info");
  EXPECT_TRUE(adapter.Release().ok());
}

TEST_F(RobloxInputNativeAdapterTest, CallsExactExportedJniSignatures) {
  RobloxInputRuntime runtime(Environment(), Symbols());
  ASSERT_TRUE(runtime.Initialize({1280, 720, 2560, 1440}, true).ok());

  EXPECT_TRUE(runtime
                  .HandleEvent(Event(platform::MouseMotionEvent{
                      90.0f, 190.0f, 4.0f, -2.0f, 0}))
                  .dispatched());
  EXPECT_TRUE(runtime
                  .HandleEvent(Event(platform::MouseButtonEvent{
                      true, SDL_BUTTON_MIDDLE, 1, 100.0f, 200.0f}))
                  .dispatched());
  EXPECT_TRUE(runtime
                  .HandleEvent(Event(platform::TouchEvent{
                      platform::TouchEvent::Action::kDown, 55, 66, 0.25f, 0.5f,
                      0.0f, 0.0f, 1.0f}))
                  .dispatched());
  EXPECT_TRUE(runtime
                  .HandleEvent(Event(
                      platform::MouseWheelEvent{0.0f, 3.0f, 100.0f, 200.0f}))
                  .dispatched());
  EXPECT_TRUE(runtime
                  .HandleEvent(Event(platform::KeyEvent{
                      true, false, SDL_SCANCODE_W, SDLK_W, 0}))
                  .dispatched());

  ASSERT_EQ(probe_.calls.size(), 5U);
  EXPECT_EQ(probe_.calls[0].name, "mouse-move");
  EXPECT_EQ(probe_.calls[0].arguments,
            (std::vector<double>{90.0, 190.0, 4.0, -2.0}));
  EXPECT_EQ(probe_.calls[1].name, "mouse-button");
  EXPECT_EQ(probe_.calls[1].arguments,
            (std::vector<double>{100.0, 200.0, JNI_TRUE, 3.0}));
  EXPECT_EQ(probe_.calls[2].name, "touch");
  EXPECT_EQ(probe_.calls[2].arguments,
            (std::vector<double>{0.0, 320.0, 360.0, 0.0, 1280.0, 720.0}));
  EXPECT_EQ(probe_.calls[3].name, "mouse-wheel");
  EXPECT_EQ(probe_.calls[3].arguments,
            (std::vector<double>{100.0, 200.0, 3.0}));
  EXPECT_EQ(probe_.calls[4].name, "key");
  EXPECT_EQ(probe_.calls[4].arguments,
            (std::vector<double>{JNI_TRUE, 17.0, 51.0, JNI_FALSE}));
  for (const NativeCall& call : probe_.calls) {
    EXPECT_EQ(call.env, vm_->GetJNIEnv());
    EXPECT_NE(call.clazz, nullptr);
  }
  EXPECT_GE(probe_.prepare_count, 4);
}

TEST_F(RobloxInputNativeAdapterTest, QueriesExactNativeMouseLockState) {
  RobloxInputRuntime runtime(Environment(), Symbols());
  ASSERT_TRUE(runtime.Initialize({1280, 720, 1280, 720}, true).ok());

  bool locked_center = false;
  ASSERT_TRUE(runtime.GetMainWindowIsMouseLockedCenter(&locked_center).ok());

  EXPECT_TRUE(locked_center);
  ASSERT_EQ(probe_.calls.size(), 1U);
  EXPECT_EQ(probe_.calls[0].name, "mouse-lock-query");
  EXPECT_EQ(probe_.calls[0].env, vm_->GetJNIEnv());
}

TEST_F(RobloxInputNativeAdapterTest,
       PlatformObserverStopsCallingNativeAfterDeactivate) {
  RobloxInputRuntime runtime(Environment(), Symbols());
  ASSERT_TRUE(runtime.Initialize({1280, 720, 1280, 720}, true).ok());
  const platform::PlatformEvent click =
      Event(platform::MouseButtonEvent{true, SDL_BUTTON_LEFT, 1, 50.0f, 60.0f});

  RobloxInputRuntime::PlatformEventCallback(&runtime, click);
  ASSERT_EQ(probe_.calls.size(), 1U);
  ASSERT_TRUE(runtime.Deactivate().ok());
  const size_t after_deactivate = probe_.calls.size();
  RobloxInputRuntime::PlatformEventCallback(&runtime, click);

  EXPECT_EQ(probe_.calls.size(), after_deactivate);
  EXPECT_FALSE(runtime.Snapshot().active);
}

TEST_F(RobloxInputNativeAdapterTest, TextNeverCallsNativeWithoutTextboxHandle) {
  RobloxInputRuntime runtime(Environment(), Symbols());
  ASSERT_TRUE(runtime.Initialize({1280, 720, 1280, 720}, true).ok());

  RobloxInputDispatchResult result =
      runtime.HandleEvent(Event(platform::TextInputEvent{"hello"}));

  EXPECT_EQ(result.state, RobloxInputDispatchState::kIgnoredUnsupported);
  EXPECT_EQ(result.status.code(), StatusCode::kUnsupported);
  EXPECT_TRUE(probe_.calls.empty());
}

TEST_F(RobloxInputNativeAdapterTest, RoutesUnicodeTextAndEnterInApkOrder) {
  RobloxInputRuntime runtime(Environment(), Symbols());
  ASSERT_TRUE(runtime.Initialize({1280, 720, 1280, 720}, true).ok());
  ASSERT_TRUE(runtime.BeginTextFocusSession({55, 9, "🙂", false, false}).ok());

  EXPECT_TRUE(
      runtime.HandleEvent(Event(platform::TextInputEvent{"é"})).dispatched());
  EXPECT_TRUE(runtime
                  .HandleEvent(Event(platform::KeyEvent{
                      true, false, SDL_SCANCODE_RETURN, SDLK_RETURN, 0}))
                  .dispatched());

  ASSERT_EQ(probe_.calls.size(), 5u);
  EXPECT_EQ(probe_.calls[0].name, "sync-text");
  EXPECT_EQ(probe_.calls[0].text, u"🙂é");
  EXPECT_EQ(probe_.calls[0].arguments[0], 3);
  EXPECT_EQ(probe_.calls[1].name, "pass-text");
  EXPECT_EQ(probe_.calls[1].arguments, (std::vector<double>{55, JNI_FALSE, 3}));
  EXPECT_EQ(probe_.calls[2].name, "sync-text");
  EXPECT_EQ(probe_.calls[3].name, "return");
  EXPECT_EQ(probe_.calls[4].name, "pass-text");
  EXPECT_EQ(probe_.calls[4].arguments, (std::vector<double>{55, JNI_TRUE, 3}));
}

TEST_F(RobloxInputNativeAdapterTest,
       FocusedTextRoutesAsciiKeyPairAndCommittedTextInSdlOrder) {
  RobloxInputRuntime runtime(Environment(), Symbols());
  ASSERT_TRUE(runtime.Initialize({1280, 720, 1280, 720}, true).ok());
  ASSERT_TRUE(runtime.BeginTextFocusSession({42, 1, "", false, false}).ok());

  EXPECT_TRUE(runtime
                  .HandleEvent(Event(platform::KeyEvent{
                      true, false, SDL_SCANCODE_A, SDLK_A, 0}))
                  .dispatched());
  EXPECT_TRUE(
      runtime.HandleEvent(Event(platform::TextInputEvent{"a"})).dispatched());
  EXPECT_TRUE(runtime
                  .HandleEvent(Event(platform::KeyEvent{
                      false, false, SDL_SCANCODE_A, SDLK_A, 0}))
                  .dispatched());

  ASSERT_EQ(probe_.calls.size(), 4u);
  EXPECT_EQ(probe_.calls[0].name, "key");
  EXPECT_EQ(probe_.calls[0].arguments,
            (std::vector<double>{JNI_TRUE, 30.0, 29.0, JNI_FALSE}));
  EXPECT_EQ(probe_.calls[1].name, "sync-text");
  EXPECT_EQ(probe_.calls[1].text, u"a");
  EXPECT_EQ(probe_.calls[1].arguments, (std::vector<double>{1.0}));
  EXPECT_EQ(probe_.calls[2].name, "pass-text");
  EXPECT_EQ(probe_.calls[2].text, u"a");
  EXPECT_EQ(probe_.calls[2].arguments,
            (std::vector<double>{42.0, JNI_FALSE, 1.0}));
  EXPECT_EQ(probe_.calls[3].name, "key");
  EXPECT_EQ(probe_.calls[3].arguments,
            (std::vector<double>{JNI_FALSE, 30.0, 29.0, JNI_FALSE}));
  for (const NativeCall& call : probe_.calls) {
    EXPECT_EQ(call.env, vm_->GetJNIEnv());
    EXPECT_NE(call.clazz, nullptr);
  }
  const RobloxInputSnapshot snapshot = runtime.Snapshot();
  EXPECT_EQ(snapshot.keyboard_events, 2u);
  EXPECT_EQ(snapshot.text_events, 1u);
  EXPECT_EQ(snapshot.ignored_text_events, 0u);
  EXPECT_EQ(snapshot.active_keys, 0u);
  EXPECT_EQ(snapshot.native_errors, 0u);
}

TEST_F(RobloxInputNativeAdapterTest,
       FocusedTextEditingPreeditUsesExactNativeSequence) {
  RobloxInputRuntime runtime(Environment(), Symbols());
  ASSERT_TRUE(runtime.Initialize({1280, 720, 1280, 720}, true).ok());
  ASSERT_TRUE(runtime.BeginTextFocusSession({42, 1, "", false, false}).ok());

  const RobloxInputDispatchResult result =
      runtime.HandleEvent(Event(platform::TextEditingEvent{"preedit", 0, 7}));

  EXPECT_EQ(result.state, RobloxInputDispatchState::kDispatched);
  EXPECT_EQ(result.kind, RobloxInputEventKind::kText);
  EXPECT_TRUE(result.status.ok());
  ASSERT_EQ(probe_.calls.size(), 2u);
  EXPECT_EQ(probe_.calls[0].name, "sync-text");
  EXPECT_EQ(probe_.calls[0].text, u"preedit");
  EXPECT_EQ(probe_.calls[0].arguments, (std::vector<double>{0.0}));
  EXPECT_EQ(probe_.calls[1].name, "pass-text");
  EXPECT_EQ(probe_.calls[1].text, u"preedit");
  EXPECT_EQ(probe_.calls[1].arguments,
            (std::vector<double>{42.0, JNI_FALSE, 0.0}));
  const RobloxInputSnapshot snapshot = runtime.Snapshot();
  EXPECT_EQ(snapshot.keyboard_events, 0u);
  EXPECT_EQ(snapshot.text_events, 1u);
  EXPECT_EQ(snapshot.ignored_text_events, 0u);
  EXPECT_EQ(snapshot.active_keys, 0u);
  EXPECT_EQ(snapshot.native_errors, 0u);
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
