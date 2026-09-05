#include <SDL3/SDL_gamepad.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "runtime/roblox_input_router.h"

namespace mocktail {
namespace runtime {
namespace {

struct GamepadCall {
  std::string kind;
  int32_t device = 0;
  int32_t code = 0;
  int32_t direction = 0;
  bool enabled = false;
  int32_t type = 0;
  std::array<float, 3> position{};
};

struct GamepadProbe {
  std::vector<GamepadCall> calls;
  std::string fail_next;

  Status Record(GamepadCall call) {
    const bool fail = !fail_next.empty() && call.kind == fail_next;
    calls.push_back(std::move(call));
    if (fail) {
      fail_next.clear();
      return Status::Error(StatusCode::kPlatformError, "injected failure");
    }
    return Status::Ok();
  }

  std::vector<GamepadCall> OfKind(const char* kind) const {
    std::vector<GamepadCall> result;
    std::copy_if(calls.begin(), calls.end(), std::back_inserter(result),
                 [kind](const GamepadCall& call) { return call.kind == kind; });
    return result;
  }
};

RobloxInputSink GamepadSink(GamepadProbe* probe) {
  RobloxInputSink sink;
  sink.gamepad = {
      probe,
      [](void* p, int32_t id, int32_t key, bool supported, int32_t type) {
        return static_cast<GamepadProbe*>(p)->Record(
            {"key-cap", id, key, 0, supported, type});
      },
      [](void* p, int32_t id, int32_t axis, int32_t direction, bool supported,
         int32_t type) {
        return static_cast<GamepadProbe*>(p)->Record(
            {"axis-cap", id, axis, direction, supported, type});
      },
      [](void* p, int32_t id, int32_t type) {
        return static_cast<GamepadProbe*>(p)->Record(
            {"connect", id, 0, 0, true, type});
      },
      [](void* p, int32_t id) {
        return static_cast<GamepadProbe*>(p)->Record({"disconnect", id});
      },
      [](void* p, int32_t id, int32_t key, bool pressed) {
        return static_cast<GamepadProbe*>(p)->Record(
            {"button", id, key, 0, pressed});
      },
      [](void* p, int32_t id, int32_t axis, float x, float y, float z) {
        return static_cast<GamepadProbe*>(p)->Record(
            {"axis", id, axis, 0, false, 0, {x, y, z}});
      }};
  return sink;
}

platform::GamepadDescriptor Descriptor(
    platform::GamepadFamily family = platform::GamepadFamily::kXbox) {
  return {family, "Fixture gamepad", (uint32_t{1} << 15) - 1,
          (uint32_t{1} << SDL_GAMEPAD_AXIS_COUNT) - 1};
}

class RobloxGamepadInputTest : public ::testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(router.Activate({1280, 720, 1280, 720, 1.0F}, true).ok());
  }

  RobloxInputDispatchResult Send(platform::PlatformEventPayload payload) {
    return router.HandleEvent({0, std::move(payload)});
  }

  void Add(int64_t id = 4000000000LL) {
    ASSERT_TRUE(Send(platform::GamepadConnectionEvent{id, true, Descriptor()})
                    .status.ok());
  }

  GamepadProbe probe;
  RobloxInputRouter router{GamepadSink(&probe)};
};

TEST_F(RobloxGamepadInputTest, AnnouncesCapabilitiesBeforeFirstAction) {
  Add();
  EXPECT_TRUE(probe.calls.empty());
  EXPECT_EQ(router.Snapshot().detected_gamepads, 1U);
  EXPECT_EQ(router.Snapshot().connected_gamepads, 0U);
  ASSERT_TRUE(Send(platform::GamepadButtonEvent{4000000000LL,
                                                SDL_GAMEPAD_BUTTON_SOUTH, true})
                  .dispatched());
  const auto connected = probe.OfKind("connect");
  ASSERT_EQ(connected.size(), 1U);
  EXPECT_EQ(connected[0].device, 1);
  EXPECT_EQ(connected[0].type, 3);
  ASSERT_GE(probe.calls.size(), 3U);
  EXPECT_EQ(probe.calls[probe.calls.size() - 2].kind, "connect");
  EXPECT_EQ(probe.calls.back().kind, "button");
  EXPECT_EQ(probe.calls.back().code, 96);
  EXPECT_TRUE(probe.calls.back().enabled);
  const auto motions = probe.OfKind("axis-cap");
  for (const auto& motion : motions) {
    if (motion.code == 15 || motion.code == 16 || motion.code == 22 ||
        motion.code == 23) {
      EXPECT_FALSE(motion.enabled);
    }
  }
  EXPECT_EQ(router.Snapshot().connected_gamepads, 1U);
}

TEST_F(RobloxGamepadInputTest, FamilyTypesAndButtonsUseTheAndroidAbi) {
  EXPECT_EQ(RobloxGamepadType(platform::GamepadFamily::kUnknown), 0);
  EXPECT_EQ(RobloxGamepadType(platform::GamepadFamily::kPlayStation4), 1);
  EXPECT_EQ(RobloxGamepadType(platform::GamepadFamily::kPlayStation5), 2);
  EXPECT_EQ(RobloxGamepadType(platform::GamepadFamily::kXbox), 3);
  EXPECT_EQ(MapSdlGamepadButtonToAndroid(SDL_GAMEPAD_BUTTON_SOUTH), 96);
  EXPECT_EQ(MapSdlGamepadButtonToAndroid(SDL_GAMEPAD_BUTTON_START), 108);
  EXPECT_EQ(MapSdlGamepadButtonToAndroid(SDL_GAMEPAD_BUTTON_BACK), 109);
  EXPECT_EQ(MapSdlGamepadButtonToAndroid(SDL_GAMEPAD_BUTTON_GUIDE), -1);
  EXPECT_EQ(MapSdlGamepadButtonToAndroid(255), -1);
}

TEST_F(RobloxGamepadInputTest,
       NeutralAxesAndRepeatedButtonsDoNotGenerateInput) {
  Add(10);
  Send(platform::GamepadAxisEvent{10, SDL_GAMEPAD_AXIS_LEFTX, 1000});
  Send(platform::GamepadAxisEvent{10, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 0});
  Send(platform::GamepadButtonEvent{10, SDL_GAMEPAD_BUTTON_SOUTH, false});
  EXPECT_TRUE(probe.calls.empty());
  Send(platform::GamepadButtonEvent{10, SDL_GAMEPAD_BUTTON_SOUTH, true});
  Send(platform::GamepadButtonEvent{10, SDL_GAMEPAD_BUTTON_SOUTH, true});
  Send(platform::GamepadButtonEvent{10, SDL_GAMEPAD_BUTTON_SOUTH, false});
  const auto buttons = probe.OfKind("button");
  ASSERT_EQ(buttons.size(), 2U);
  EXPECT_TRUE(buttons[0].enabled);
  EXPECT_FALSE(buttons[1].enabled);
}

TEST_F(RobloxGamepadInputTest, StickVectorsFlipYAndTriggersUseZ) {
  Add(10);
  ASSERT_TRUE(
      Send(platform::GamepadAxisEvent{10, SDL_GAMEPAD_AXIS_LEFTY, -32768})
          .dispatched());
  auto axes = probe.OfKind("axis");
  ASSERT_EQ(axes.size(), 2U);
  EXPECT_EQ(axes[0].code, 0);
  EXPECT_EQ(axes[1].code, 1);
  EXPECT_FLOAT_EQ(axes[0].position[0], 0);
  EXPECT_FLOAT_EQ(axes[0].position[1], 1);
  EXPECT_EQ(axes[0].position, axes[1].position);
  Send(platform::GamepadAxisEvent{10, SDL_GAMEPAD_AXIS_LEFTX, 32767});
  axes = probe.OfKind("axis");
  EXPECT_NEAR(axes.back().position[0], std::sqrt(0.5F), 0.00001F);
  EXPECT_NEAR(axes.back().position[1], std::sqrt(0.5F), 0.00001F);
  Send(platform::GamepadAxisEvent{10, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767});
  axes = probe.OfKind("axis");
  EXPECT_EQ(axes.back().code, 18);
  EXPECT_FLOAT_EQ(axes.back().position[0], 0);
  EXPECT_FLOAT_EQ(axes.back().position[1], 0);
  EXPECT_FLOAT_EQ(axes.back().position[2], 1);
}

TEST_F(RobloxGamepadInputTest,
       FocusLossNeutralizesAndBackgroundInputCannotRefocus) {
  Add(10);
  Send(platform::GamepadButtonEvent{10, SDL_GAMEPAD_BUTTON_SOUTH, true});
  Send(platform::GamepadAxisEvent{10, SDL_GAMEPAD_AXIS_LEFTX, 32767});
  Send(platform::WindowFocusEvent{false});
  const auto buttons = probe.OfKind("button");
  ASSERT_EQ(buttons.size(), 2U);
  EXPECT_FALSE(buttons.back().enabled);
  const auto axes = probe.OfKind("axis");
  ASSERT_FALSE(axes.empty());
  EXPECT_EQ(axes.back().position, (std::array<float, 3>{}));
  const auto count = probe.calls.size();
  EXPECT_EQ(
      Send(platform::GamepadButtonEvent{10, SDL_GAMEPAD_BUTTON_SOUTH, true})
          .state,
      RobloxInputDispatchState::kIgnoredUnfocused);
  Send(platform::GamepadAxisEvent{10, SDL_GAMEPAD_AXIS_LEFTX, 32767});
  EXPECT_EQ(probe.calls.size(), count);
  EXPECT_FALSE(router.Snapshot().focused);
  Send(platform::WindowFocusEvent{true});
  EXPECT_TRUE(
      Send(platform::GamepadButtonEvent{10, SDL_GAMEPAD_BUTTON_SOUTH, true})
          .dispatched());
  EXPECT_EQ(probe.OfKind("connect").size(), 1U);
}

TEST_F(RobloxGamepadInputTest,
       DisconnectReleasesBeforeRemovalAndOtherPadsSurvive) {
  Add(10);
  Add(20);
  Send(platform::GamepadButtonEvent{10, SDL_GAMEPAD_BUTTON_SOUTH, true});
  Send(platform::GamepadButtonEvent{20, SDL_GAMEPAD_BUTTON_EAST, true});
  probe.calls.clear();
  Send(platform::GamepadConnectionEvent{10, false});
  ASSERT_FALSE(probe.calls.empty());
  EXPECT_EQ(probe.calls.front().kind, "button");
  EXPECT_FALSE(probe.calls.front().enabled);
  EXPECT_EQ(probe.calls.back().kind, "disconnect");
  for (const auto& call : probe.calls) {
    EXPECT_EQ(call.device, 1);
  }
  EXPECT_EQ(router.Snapshot().connected_gamepads, 1U);
  EXPECT_EQ(router.Snapshot().detected_gamepads, 1U);
  Add(30);
  Send(platform::GamepadButtonEvent{30, SDL_GAMEPAD_BUTTON_SOUTH, true});
  EXPECT_EQ(probe.OfKind("connect").back().device, 1);
}

TEST_F(RobloxGamepadInputTest, RemappingReconnectsWithUpdatedFamily) {
  Add(10);
  Send(platform::GamepadButtonEvent{10, SDL_GAMEPAD_BUTTON_SOUTH, true});
  Send(platform::GamepadConnectionEvent{
      10, true, Descriptor(platform::GamepadFamily::kPlayStation5), true});
  EXPECT_EQ(probe.OfKind("disconnect").size(), 1U);
  EXPECT_EQ(router.Snapshot().connected_gamepads, 0U);
  Send(platform::GamepadButtonEvent{10, SDL_GAMEPAD_BUTTON_SOUTH, true});
  EXPECT_EQ(probe.OfKind("connect").back().type, 2);
}

TEST_F(RobloxGamepadInputTest, FailedRegistrationRollsBackAndCanBeRetried) {
  Add(10);
  probe.fail_next = "connect";
  EXPECT_EQ(
      Send(platform::GamepadButtonEvent{10, SDL_GAMEPAD_BUTTON_SOUTH, true})
          .state,
      RobloxInputDispatchState::kNativeError);
  EXPECT_EQ(router.Snapshot().connected_gamepads, 0U);
  EXPECT_TRUE(probe.OfKind("button").empty());
  ASSERT_FALSE(probe.calls.empty());
  EXPECT_EQ(probe.calls.back().kind, "disconnect");
  EXPECT_TRUE(
      Send(platform::GamepadButtonEvent{10, SDL_GAMEPAD_BUTTON_SOUTH, true})
          .dispatched());
  EXPECT_EQ(router.Snapshot().connected_gamepads, 1U);
}

TEST_F(RobloxGamepadInputTest, MissingApiAndUnknownInputsAreRejected) {
  RobloxInputRouter unsupported({});
  ASSERT_TRUE(unsupported.Activate({1280, 720, 1280, 720, 1}, true).ok());
  EXPECT_EQ(
      unsupported
          .HandleEvent(
              {0, platform::GamepadConnectionEvent{10, true, Descriptor()}})
          .state,
      RobloxInputDispatchState::kIgnoredUnsupported);
  Add(10);
  EXPECT_EQ(Send(platform::GamepadButtonEvent{10, 255, true}).state,
            RobloxInputDispatchState::kIgnoredUnsupported);
  EXPECT_EQ(Send(platform::GamepadAxisEvent{10, 255, 32767}).state,
            RobloxInputDispatchState::kIgnoredUnsupported);
  EXPECT_TRUE(probe.calls.empty());
  EXPECT_TRUE(router.Deactivate().ok());
  EXPECT_EQ(
      Send(platform::GamepadButtonEvent{10, SDL_GAMEPAD_BUTTON_SOUTH, true})
          .state,
      RobloxInputDispatchState::kIgnoredInactive);
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
