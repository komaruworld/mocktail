#include "runtime/roblox_input_router.h"

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keycode.h>
#include <SDL3/SDL_mouse.h>
#include <SDL3/SDL_scancode.h>
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "mocktail/platform/sdl_event_converter.h"

namespace mocktail {
namespace runtime {
namespace {

struct MouseMoveCall {
  float x = 0.0f;
  float y = 0.0f;
  float delta_x = 0.0f;
  float delta_y = 0.0f;
};

struct MouseButtonCall {
  float x = 0.0f;
  float y = 0.0f;
  bool pressed = false;
  int32_t button = -1;
};

struct MouseWheelCall {
  float x = 0.0f;
  float y = 0.0f;
  float delta_y = 0.0f;
};

struct TouchCall {
  int32_t pointer_id = -1;
  float x = 0.0f;
  float y = 0.0f;
  int32_t action = -1;
  int32_t width = 0;
  int32_t height = 0;
};

struct KeyCall {
  bool pressed = false;
  int32_t scan_code = 0;
  int32_t key_code = 0;
  bool repeat = false;
};

struct Probe {
  std::vector<MouseMoveCall> mouse_moves;
  std::vector<MouseButtonCall> mouse_buttons;
  std::vector<MouseWheelCall> mouse_wheels;
  std::vector<TouchCall> touches;
  std::vector<KeyCall> keys;
  std::vector<std::string> text_operations;
  std::vector<std::string> ordered_operations;
  Status next_status = Status::Ok();

  size_t CallCount() const {
    return mouse_moves.size() + mouse_buttons.size() + mouse_wheels.size() +
           touches.size() + keys.size() + text_operations.size();
  }
};

class ClipboardProbe final : public platform::TextClipboard {
 public:
  Status ReadText(std::string* text) override {
    ++read_calls;
    *text = value;
    return Status::Ok();
  }

  Status WriteText(const std::string& text) override {
    value = text;
    return Status::Ok();
  }

  std::string value;
  int read_calls = 0;
};

Status ConsumeStatus(Probe* probe) {
  Status status = probe->next_status;
  probe->next_status = Status::Ok();
  return status;
}

Status MouseMove(void* context, float x, float y, float delta_x,
                 float delta_y) {
  auto* probe = static_cast<Probe*>(context);
  probe->mouse_moves.push_back({x, y, delta_x, delta_y});
  return ConsumeStatus(probe);
}

Status MouseButton(void* context, float x, float y, bool pressed,
                   int32_t button) {
  auto* probe = static_cast<Probe*>(context);
  probe->mouse_buttons.push_back({x, y, pressed, button});
  return ConsumeStatus(probe);
}

Status MouseWheel(void* context, float x, float y, float delta_y) {
  auto* probe = static_cast<Probe*>(context);
  probe->mouse_wheels.push_back({x, y, delta_y});
  return ConsumeStatus(probe);
}

Status Touch(void* context, int32_t pointer_id, float x, float y,
             int32_t action, int32_t width, int32_t height) {
  auto* probe = static_cast<Probe*>(context);
  probe->touches.push_back({pointer_id, x, y, action, width, height});
  return ConsumeStatus(probe);
}

Status Key(void* context, bool pressed, int32_t scan_code, int32_t key_code,
           bool repeat) {
  auto* probe = static_cast<Probe*>(context);
  probe->keys.push_back({pressed, scan_code, key_code, repeat});
  probe->ordered_operations.push_back(pressed ? "key-down" : "key-up");
  return ConsumeStatus(probe);
}

Status SyncText(void* context, const char*, std::size_t, int32_t) {
  auto* probe = static_cast<Probe*>(context);
  probe->text_operations.push_back("sync");
  probe->ordered_operations.push_back("sync");
  return ConsumeStatus(probe);
}

Status PassText(void* context, int64_t, const char*, std::size_t, bool,
                int32_t) {
  auto* probe = static_cast<Probe*>(context);
  probe->text_operations.push_back("pass");
  probe->ordered_operations.push_back("pass");
  return ConsumeStatus(probe);
}

Status TextHandle(void* context, int64_t) {
  auto* probe = static_cast<Probe*>(context);
  probe->text_operations.push_back("handle");
  probe->ordered_operations.push_back("handle");
  return ConsumeStatus(probe);
}

RobloxInputSink Sink(Probe* probe) {
  RobloxInputSink sink = {probe, MouseMove, MouseButton, MouseWheel,
                          Touch, Key,       {}};
  sink.text = {probe, SyncText, PassText, TextHandle, TextHandle};
  return sink;
}

platform::PlatformEvent Event(platform::PlatformEventPayload payload) {
  return {123, std::move(payload)};
}

class RobloxInputRouterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(router_.Activate({1280, 720, 2560, 1440}, true).ok());
  }

  Probe probe_;
  RobloxInputRouter router_{Sink(&probe_)};
};

TEST(RobloxInputMappingTest, MapsAndroidMouseButtonsExactlyLikeApk) {
  EXPECT_EQ(MapSdlMouseButtonToAndroid(SDL_BUTTON_LEFT), 0);
  EXPECT_EQ(MapSdlMouseButtonToAndroid(SDL_BUTTON_RIGHT), 1);
  EXPECT_EQ(MapSdlMouseButtonToAndroid(SDL_BUTTON_MIDDLE), 3);
  EXPECT_EQ(MapSdlMouseButtonToAndroid(SDL_BUTTON_X1), 7);
  EXPECT_EQ(MapSdlMouseButtonToAndroid(SDL_BUTTON_X2), 15);
  EXPECT_EQ(MapSdlMouseButtonToAndroid(42), -1);
}

TEST(RobloxInputMappingTest, MapsSdlUsbKeysToLinuxAndAndroidCodes) {
  EXPECT_EQ(MapSdlKeyToAndroid(SDL_SCANCODE_W).scan_code, 17);
  EXPECT_EQ(MapSdlKeyToAndroid(SDL_SCANCODE_W).key_code, 51);
  EXPECT_EQ(MapSdlKeyToAndroid(SDL_SCANCODE_SPACE).scan_code, 57);
  EXPECT_EQ(MapSdlKeyToAndroid(SDL_SCANCODE_SPACE).key_code, 62);
  EXPECT_EQ(MapSdlKeyToAndroid(SDL_SCANCODE_LSHIFT).scan_code, 42);
  EXPECT_EQ(MapSdlKeyToAndroid(SDL_SCANCODE_LSHIFT).key_code, 59);
  EXPECT_FALSE(MapSdlKeyToAndroid(SDL_SCANCODE_UNKNOWN).valid());
}

TEST_F(RobloxInputRouterTest, RoutesMouseMotionButtonAndVerticalWheel) {
  EXPECT_TRUE(router_
                  .HandleEvent(Event(platform::MouseMotionEvent{
                      100.0f, 80.0f, 5.0f, -3.0f, 0}))
                  .dispatched());
  EXPECT_TRUE(router_
                  .HandleEvent(Event(platform::MouseButtonEvent{
                      true, SDL_BUTTON_RIGHT, 1, 100.0f, 80.0f}))
                  .dispatched());
  EXPECT_TRUE(router_
                  .HandleEvent(Event(
                      platform::MouseWheelEvent{0.0f, -2.0f, -4.0f, -5.0f}))
                  .dispatched());

  ASSERT_EQ(probe_.mouse_moves.size(), 1U);
  EXPECT_FLOAT_EQ(probe_.mouse_moves[0].delta_x, 5.0f);
  ASSERT_EQ(probe_.mouse_buttons.size(), 1U);
  EXPECT_EQ(probe_.mouse_buttons[0].button, 1);
  ASSERT_EQ(probe_.mouse_wheels.size(), 1U);
  EXPECT_FLOAT_EQ(probe_.mouse_wheels[0].x, 0.0f);
  EXPECT_FLOAT_EQ(probe_.mouse_wheels[0].y, 0.0f);
  EXPECT_FLOAT_EQ(probe_.mouse_wheels[0].delta_y, -2.0f);
}

TEST_F(RobloxInputRouterTest, ScalesNormalizedTouchAndKeepsStablePointerIds) {
  EXPECT_TRUE(router_
                  .HandleEvent(Event(platform::TouchEvent{
                      platform::TouchEvent::Action::kDown, 10, 1001, 0.25f,
                      0.5f, 0.0f, 0.0f, 1.0f}))
                  .dispatched());
  EXPECT_TRUE(router_
                  .HandleEvent(Event(platform::TouchEvent{
                      platform::TouchEvent::Action::kDown, 10, 9000000000LL,
                      0.75f, 0.25f, 0.0f, 0.0f, 1.0f}))
                  .dispatched());
  EXPECT_TRUE(router_
                  .HandleEvent(Event(platform::TouchEvent{
                      platform::TouchEvent::Action::kMove, 10, 1001, 0.5f, 1.0f,
                      0.25f, 0.5f, 1.0f}))
                  .dispatched());
  EXPECT_TRUE(router_
                  .HandleEvent(Event(platform::TouchEvent{
                      platform::TouchEvent::Action::kUp, 10, 1001, 0.5f, 1.0f,
                      0.0f, 0.0f, 0.0f}))
                  .dispatched());
  EXPECT_TRUE(router_
                  .HandleEvent(Event(platform::TouchEvent{
                      platform::TouchEvent::Action::kDown, 20, 2002, 0.1f, 0.1f,
                      0.0f, 0.0f, 1.0f}))
                  .dispatched());

  ASSERT_EQ(probe_.touches.size(), 5U);
  EXPECT_EQ(probe_.touches[0].pointer_id, 0);
  EXPECT_EQ(probe_.touches[0].action, 0);
  EXPECT_FLOAT_EQ(probe_.touches[0].x, 320.0f);
  EXPECT_FLOAT_EQ(probe_.touches[0].y, 360.0f);
  EXPECT_EQ(probe_.touches[0].width, 1280);
  EXPECT_EQ(probe_.touches[0].height, 720);
  EXPECT_EQ(probe_.touches[1].pointer_id, 1);
  EXPECT_EQ(probe_.touches[2].pointer_id, 0);
  EXPECT_EQ(probe_.touches[2].action, 1);
  EXPECT_EQ(probe_.touches[3].pointer_id, 0);
  EXPECT_EQ(probe_.touches[3].action, 2);
  EXPECT_EQ(probe_.touches[4].pointer_id, 0);
  EXPECT_EQ(probe_.touches[4].action, 0);
}

TEST_F(RobloxInputRouterTest, ResizeChangesTouchScaleAndNativeViewport) {
  RobloxInputDispatchResult resize = router_.HandleEvent(
      Event(platform::WindowResizedEvent{640, 360, 1920, 1080}));
  EXPECT_EQ(resize.state, RobloxInputDispatchState::kStateUpdated);

  EXPECT_TRUE(router_
                  .HandleEvent(Event(
                      platform::TouchEvent{platform::TouchEvent::Action::kDown,
                                           1, 2, 0.5f, 0.5f, 0.0f, 0.0f, 1.0f}))
                  .dispatched());

  ASSERT_EQ(probe_.touches.size(), 1U);
  EXPECT_FLOAT_EQ(probe_.touches[0].x, 320.0f);
  EXPECT_FLOAT_EQ(probe_.touches[0].y, 180.0f);
  EXPECT_EQ(probe_.touches[0].width, 640);
  EXPECT_EQ(probe_.touches[0].height, 360);
  EXPECT_EQ(router_.Snapshot().viewport.pixel_width, 1920);
}

TEST_F(RobloxInputRouterTest, RoutesMappedHardwareKeyAndRepeat) {
  RobloxInputDispatchResult down = router_.HandleEvent(
      Event(platform::KeyEvent{true, false, SDL_SCANCODE_W, SDLK_W, 0}));
  RobloxInputDispatchResult repeat = router_.HandleEvent(
      Event(platform::KeyEvent{true, true, SDL_SCANCODE_W, SDLK_W, 0}));
  RobloxInputDispatchResult up = router_.HandleEvent(
      Event(platform::KeyEvent{false, false, SDL_SCANCODE_W, SDLK_W, 0}));

  EXPECT_TRUE(down.dispatched());
  EXPECT_TRUE(repeat.dispatched());
  EXPECT_TRUE(up.dispatched());
  ASSERT_EQ(probe_.keys.size(), 3U);
  EXPECT_EQ(probe_.keys[0].scan_code, 17);
  EXPECT_EQ(probe_.keys[0].key_code, 51);
  EXPECT_FALSE(probe_.keys[0].repeat);
  EXPECT_TRUE(probe_.keys[1].repeat);
  EXPECT_FALSE(probe_.keys[2].pressed);
}

TEST_F(RobloxInputRouterTest, FocusLossReleasesAllPressedInputs) {
  ASSERT_TRUE(router_
                  .HandleEvent(Event(platform::MouseButtonEvent{
                      true, SDL_BUTTON_LEFT, 1, 12.0f, 34.0f}))
                  .dispatched());
  ASSERT_TRUE(router_
                  .HandleEvent(Event(
                      platform::TouchEvent{platform::TouchEvent::Action::kDown,
                                           7, 8, 0.1f, 0.2f, 0.0f, 0.0f, 1.0f}))
                  .dispatched());
  ASSERT_TRUE(router_
                  .HandleEvent(Event(platform::KeyEvent{
                      true, false, SDL_SCANCODE_SPACE, SDLK_SPACE, 0}))
                  .dispatched());

  RobloxInputDispatchResult focus =
      router_.HandleEvent(Event(platform::WindowFocusEvent{false}));
  EXPECT_EQ(focus.state, RobloxInputDispatchState::kStateUpdated);
  ASSERT_EQ(probe_.mouse_buttons.size(), 2U);
  EXPECT_FALSE(probe_.mouse_buttons.back().pressed);
  ASSERT_EQ(probe_.touches.size(), 2U);
  EXPECT_EQ(probe_.touches.back().action, 2);
  ASSERT_EQ(probe_.keys.size(), 2U);
  EXPECT_FALSE(probe_.keys.back().pressed);
  EXPECT_EQ(router_.Snapshot().active_mouse_buttons, 0U);
  EXPECT_EQ(router_.Snapshot().active_touches, 0U);
  EXPECT_EQ(router_.Snapshot().active_keys, 0U);

  const size_t before = probe_.CallCount();
  EXPECT_EQ(router_
                .HandleEvent(Event(
                    platform::MouseMotionEvent{1.0f, 2.0f, 1.0f, 1.0f, 0}))
                .state,
            RobloxInputDispatchState::kIgnoredUnfocused);
  EXPECT_EQ(probe_.CallCount(), before);
}

TEST_F(RobloxInputRouterTest, TextFailsClosedWithoutTextboxHandle) {
  RobloxInputDispatchResult result =
      router_.HandleEvent(Event(platform::TextInputEvent{"hello"}));

  EXPECT_EQ(result.state, RobloxInputDispatchState::kIgnoredUnsupported);
  EXPECT_EQ(result.kind, RobloxInputEventKind::kText);
  EXPECT_EQ(result.status.code(), StatusCode::kUnsupported);
  EXPECT_EQ(router_.Snapshot().ignored_text_events, 1U);
  EXPECT_EQ(probe_.CallCount(), 0U);
}

TEST_F(RobloxInputRouterTest, FocusedTextSuppressesDuplicateHardwareEditKeys) {
  ASSERT_TRUE(router_.BeginTextFocusSession({42, 1, "a", false, false}).ok());
  EXPECT_TRUE(
      router_.HandleEvent(Event(platform::TextInputEvent{"b"})).dispatched());
  EXPECT_TRUE(router_
                  .HandleEvent(Event(platform::KeyEvent{
                      true, false, SDL_SCANCODE_BACKSPACE, SDLK_BACKSPACE, 0}))
                  .dispatched());
  const auto key_up = router_.HandleEvent(Event(platform::KeyEvent{
      false, false, SDL_SCANCODE_BACKSPACE, SDLK_BACKSPACE, 0}));
  EXPECT_EQ(key_up.state, RobloxInputDispatchState::kStateUpdated);
  EXPECT_TRUE(probe_.keys.empty());
  EXPECT_EQ(probe_.text_operations,
            (std::vector<std::string>{"sync", "pass", "sync", "pass"}));
  EXPECT_EQ(router_.Snapshot().text_events, 2u);
}

TEST(RobloxInputRouterClipboardTest,
     CtrlVPastesWithoutForwardingTheLetterKeyToRoblox) {
  Probe probe;
  ClipboardProbe clipboard;
  clipboard.value = u8"clipboard🙂";
  RobloxInputRouter router(Sink(&probe), {}, &clipboard);
  ASSERT_TRUE(router.Activate({1280, 720, 2560, 1440}, true).ok());
  ASSERT_TRUE(router.BeginTextFocusSession({42, 1, "old", false, false}).ok());
  SDL_Event source{};
  source.type = SDL_EVENT_KEY_DOWN;
  source.key.down = true;
  source.key.scancode = SDL_SCANCODE_V;
  source.key.key = SDLK_V;
  source.key.mod = SDL_KMOD_CTRL;
  platform::PlatformEvent converted;
  auto* non_owning_window = reinterpret_cast<SDL_Window*>(0x1);
  ASSERT_TRUE(
      platform::ConvertSdlEvent(non_owning_window, source, &converted));
  const RobloxInputDispatchResult down = router.HandleEvent(converted);

  source.type = SDL_EVENT_KEY_UP;
  source.key.down = false;
  source.key.mod = SDL_KMOD_NONE;
  ASSERT_TRUE(
      platform::ConvertSdlEvent(non_owning_window, source, &converted));
  const RobloxInputDispatchResult up = router.HandleEvent(converted);

  EXPECT_TRUE(down.dispatched());
  EXPECT_EQ(down.kind, RobloxInputEventKind::kText);
  EXPECT_EQ(up.state, RobloxInputDispatchState::kStateUpdated);
  EXPECT_EQ(clipboard.read_calls, 1);
  EXPECT_TRUE(probe.keys.empty());
  EXPECT_EQ(probe.text_operations,
            (std::vector<std::string>{"sync", "pass"}));
  EXPECT_EQ(router.Snapshot().text_events, 1u);
  EXPECT_EQ(router.Snapshot().keyboard_events, 0u);
}

TEST_F(RobloxInputRouterTest,
       FocusedTextRoutesAsciiKeyPairAndCommittedTextInSdlOrder) {
  ASSERT_TRUE(router_.BeginTextFocusSession({42, 1, "", false, false}).ok());

  const RobloxInputDispatchResult down = router_.HandleEvent(
      Event(platform::KeyEvent{true, false, SDL_SCANCODE_A, SDLK_A, 0}));
  const RobloxInputDispatchResult text =
      router_.HandleEvent(Event(platform::TextInputEvent{"a"}));
  const RobloxInputDispatchResult up = router_.HandleEvent(
      Event(platform::KeyEvent{false, false, SDL_SCANCODE_A, SDLK_A, 0}));

  EXPECT_TRUE(down.dispatched());
  EXPECT_EQ(down.kind, RobloxInputEventKind::kKeyboard);
  EXPECT_TRUE(text.dispatched());
  EXPECT_EQ(text.kind, RobloxInputEventKind::kText);
  EXPECT_TRUE(up.dispatched());
  EXPECT_EQ(up.kind, RobloxInputEventKind::kKeyboard);
  ASSERT_EQ(probe_.keys.size(), 2u);
  EXPECT_TRUE(probe_.keys[0].pressed);
  EXPECT_EQ(probe_.keys[0].scan_code, 30);
  EXPECT_EQ(probe_.keys[0].key_code, 29);
  EXPECT_FALSE(probe_.keys[0].repeat);
  EXPECT_FALSE(probe_.keys[1].pressed);
  EXPECT_EQ(probe_.keys[1].scan_code, 30);
  EXPECT_EQ(probe_.keys[1].key_code, 29);
  EXPECT_FALSE(probe_.keys[1].repeat);
  EXPECT_EQ(probe_.ordered_operations,
            (std::vector<std::string>{"key-down", "sync", "pass", "key-up"}));
  const RobloxInputSnapshot snapshot = router_.Snapshot();
  EXPECT_EQ(snapshot.keyboard_events, 2u);
  EXPECT_EQ(snapshot.text_events, 1u);
  EXPECT_EQ(snapshot.ignored_text_events, 0u);
  EXPECT_EQ(snapshot.active_keys, 0u);
  EXPECT_EQ(snapshot.native_errors, 0u);
}

TEST_F(RobloxInputRouterTest, FocusedTextEditingPreeditStreamsToNative) {
  ASSERT_TRUE(router_.BeginTextFocusSession({42, 1, "", false, false}).ok());

  const RobloxInputDispatchResult result =
      router_.HandleEvent(Event(platform::TextEditingEvent{"preedit", 0, 7}));

  EXPECT_EQ(result.state, RobloxInputDispatchState::kDispatched);
  EXPECT_EQ(result.kind, RobloxInputEventKind::kText);
  EXPECT_TRUE(result.status.ok());
  EXPECT_TRUE(probe_.keys.empty());
  EXPECT_EQ(probe_.text_operations, (std::vector<std::string>{"sync", "pass"}));
  EXPECT_EQ(probe_.ordered_operations,
            (std::vector<std::string>{"sync", "pass"}));
  const RobloxInputSnapshot snapshot = router_.Snapshot();
  EXPECT_EQ(snapshot.keyboard_events, 0u);
  EXPECT_EQ(snapshot.text_events, 1u);
  EXPECT_EQ(snapshot.ignored_text_events, 0u);
  EXPECT_EQ(snapshot.active_keys, 0u);
  EXPECT_EQ(snapshot.native_errors, 0u);
}

TEST_F(RobloxInputRouterTest, WindowFocusLossReleasesFocusedTextbox) {
  ASSERT_TRUE(
      router_.BeginTextFocusSession({42, 1, "secret", false, false}).ok());
  const auto result =
      router_.HandleEvent(Event(platform::WindowFocusEvent{false}));
  EXPECT_EQ(result.state, RobloxInputDispatchState::kStateUpdated);
  EXPECT_EQ(probe_.text_operations, (std::vector<std::string>{"handle"}));
  const auto text = router_.HandleEvent(Event(platform::TextInputEvent{"x"}));
  EXPECT_EQ(text.state, RobloxInputDispatchState::kIgnoredUnfocused);
}

TEST_F(RobloxInputRouterTest, DeactivatePreventsAllLaterNativeCalls) {
  ASSERT_TRUE(router_.Deactivate().ok());
  const size_t before = probe_.CallCount();

  RobloxInputDispatchResult result = router_.HandleEvent(Event(
      platform::MouseButtonEvent{true, SDL_BUTTON_LEFT, 1, 100.0f, 100.0f}));

  EXPECT_EQ(result.state, RobloxInputDispatchState::kIgnoredInactive);
  EXPECT_EQ(probe_.CallCount(), before);
}

TEST_F(RobloxInputRouterTest,
       ConvertedSdlClickTraversesTheProductionTypedPath) {
  SDL_Event source{};
  source.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
  source.button.down = true;
  source.button.button = SDL_BUTTON_LEFT;
  source.button.clicks = 1;
  source.button.x = 321.0f;
  source.button.y = 123.0f;
  platform::PlatformEvent converted;
  auto* non_owning_window = reinterpret_cast<SDL_Window*>(0x1);

  ASSERT_TRUE(platform::ConvertSdlEvent(non_owning_window, source, &converted));
  RobloxInputDispatchResult result = router_.HandleEvent(converted);

  ASSERT_TRUE(result.dispatched());
  ASSERT_EQ(probe_.mouse_buttons.size(), 1U);
  EXPECT_FLOAT_EQ(probe_.mouse_buttons[0].x, 321.0f);
  EXPECT_FLOAT_EQ(probe_.mouse_buttons[0].y, 123.0f);
  EXPECT_TRUE(probe_.mouse_buttons[0].pressed);
  EXPECT_EQ(probe_.mouse_buttons[0].button, 0);
}

TEST_F(RobloxInputRouterTest, NativeFailuresAreObservableAndCounted) {
  probe_.next_status =
      Status::Error(StatusCode::kPlatformError, "injected failure");

  RobloxInputDispatchResult result = router_.HandleEvent(
      Event(platform::MouseMotionEvent{10.0f, 20.0f, 1.0f, 2.0f, 0}));

  EXPECT_EQ(result.state, RobloxInputDispatchState::kNativeError);
  EXPECT_EQ(result.status.code(), StatusCode::kPlatformError);
  EXPECT_EQ(router_.Snapshot().native_errors, 1U);
  EXPECT_EQ(router_.Snapshot().mouse_events, 0U);
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
