#include "mocktail/vr/xr_actions.h"

#include <gtest/gtest.h>
#include <openxr/openxr.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <string>

#include "runtime/roblox_input_router.h"
#include "runtime/roblox_xr_controller_input.h"

namespace {
using namespace mocktail::vr;
template <class T> T Handle(uintptr_t n) { return reinterpret_cast<T>(n); }
std::map<XrAction, std::string> names;
std::map<std::string, XrPath> paths;
uintptr_t next_handle;
bool focused, input_active, simple, touching;
float trigger, squeeze, stick_y;
bool click;
int syncs, stops, vibrations;
XrActionSet action_set;
XrTime located_time;
float last_amplitude;
XrDuration last_duration;
XrPath last_haptic_hand;
int simple_haptic_bindings;
std::map<XrPath, std::vector<XrPath>> suggested_paths;
} // namespace
extern "C" {
XRAPI_ATTR XrResult XRAPI_CALL
xrResultToString(XrInstance, XrResult, char buffer[XR_MAX_RESULT_STRING_SIZE]) {
  std::strcpy(buffer, "fake");
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrStringToPath(XrInstance, const char *text,
                                              XrPath *out) {
  auto &p = paths[text];
  if (!p)
    p = ++next_handle;
  *out = p;
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrPathToString(XrInstance, XrPath path,
                                              uint32_t cap, uint32_t *count,
                                              char *out) {
  for (auto &p : paths)
    if (p.second == path) {
      *count = p.first.size() + 1;
      if (cap >= *count)
        std::strcpy(out, p.first.c_str());
      return XR_SUCCESS;
    }
  return XR_ERROR_PATH_INVALID;
}
XRAPI_ATTR XrResult XRAPI_CALL xrCreateActionSet(XrInstance,
                                                 const XrActionSetCreateInfo *,
                                                 XrActionSet *out) {
  *out = action_set = Handle<XrActionSet>(++next_handle);
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrDestroyActionSet(XrActionSet) {
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrCreateAction(XrActionSet,
                                              const XrActionCreateInfo *info,
                                              XrAction *out) {
  *out = Handle<XrAction>(++next_handle);
  names[*out] = info->actionName;
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrDestroyAction(XrAction) { return XR_SUCCESS; }
XRAPI_ATTR XrResult XRAPI_CALL xrSuggestInteractionProfileBindings(
    XrInstance, const XrInteractionProfileSuggestedBinding *info) {
  for (std::uint32_t i = 0; i < info->countSuggestedBindings; ++i) {
    suggested_paths[info->interactionProfile].push_back(
        info->suggestedBindings[i].binding);
  }
  if (info->interactionProfile ==
      paths["/interaction_profiles/khr/simple_controller"])
    for (std::uint32_t i = 0; i < info->countSuggestedBindings; ++i)
      if (names[info->suggestedBindings[i].action] == "haptic")
        ++simple_haptic_bindings;
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrAttachSessionActionSets(
    XrSession, const XrSessionActionSetsAttachInfo *info) {
  EXPECT_EQ(info->countActionSets, 1u);
  EXPECT_EQ(info->actionSets[0], action_set);
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL
xrCreateActionSpace(XrSession, const XrActionSpaceCreateInfo *, XrSpace *out) {
  *out = Handle<XrSpace>(++next_handle);
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrDestroySpace(XrSpace) { return XR_SUCCESS; }
XRAPI_ATTR XrResult XRAPI_CALL xrSyncActions(XrSession,
                                             const XrActionsSyncInfo *info) {
  ++syncs;
  EXPECT_EQ(info->countActiveActionSets, 1u);
  if (info->countActiveActionSets != 1 || !info->activeActionSets)
    return XR_ERROR_VALIDATION_FAILURE;
  EXPECT_EQ(info->activeActionSets[0].actionSet, action_set);
  EXPECT_EQ(info->activeActionSets[0].subactionPath, XR_NULL_PATH);
  return focused ? XR_SUCCESS : XR_SESSION_NOT_FOCUSED;
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetCurrentInteractionProfile(
    XrSession, XrPath, XrInteractionProfileState *out) {
  return xrStringToPath(nullptr,
                        simple
                            ? "/interaction_profiles/khr/simple_controller"
                            : "/interaction_profiles/oculus/touch_controller",
                        &out->interactionProfile);
}
XRAPI_ATTR XrResult XRAPI_CALL xrLocateSpace(XrSpace, XrSpace, XrTime time,
                                             XrSpaceLocation *out) {
  located_time = time;
  out->locationFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT |
                       XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
  out->pose.orientation.w = 1;
  out->pose.position.x = .1f;
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStatePose(
    XrSession, const XrActionStateGetInfo *, XrActionStatePose *out) {
  out->isActive = input_active;
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateFloat(
    XrSession, const XrActionStateGetInfo *info, XrActionStateFloat *out) {
  out->isActive = input_active && !simple;
  out->currentState = names[info->action] == "trigger" ? trigger : squeeze;
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateVector2f(
    XrSession, const XrActionStateGetInfo *, XrActionStateVector2f *out) {
  out->isActive = input_active && !simple;
  out->currentState = {0, stick_y};
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateBoolean(
    XrSession, const XrActionStateGetInfo *info, XrActionStateBoolean *out) {
  out->isActive =
      input_active && simple && names[info->action] == "trigger_click";
  if (names[info->action] == "trigger_touch") {
    out->isActive = input_active && !simple;
    out->currentState = touching;
  } else
    out->currentState = click;
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL
xrStopHapticFeedback(XrSession, const XrHapticActionInfo *) {
  ++stops;
  return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL xrApplyHapticFeedback(
    XrSession, const XrHapticActionInfo *info, const XrHapticBaseHeader *base) {
  const auto *vibration = reinterpret_cast<const XrHapticVibration *>(base);
  last_amplitude = vibration->amplitude;
  last_duration = vibration->duration;
  last_haptic_hand = info->subactionPath;
  ++vibrations;
  return XR_SUCCESS;
}
}
namespace {
class XrActionsProduction : public testing::Test {
protected:
  XrActions actions;
  void SetUp() override {
    names.clear();
    paths.clear();
    suggested_paths.clear();
    next_handle = 100;
    focused = input_active = true;
    simple = click = touching = false;
    trigger = squeeze = stick_y = 0;
    syncs = stops = vibrations = simple_haptic_bindings = 0;
    last_amplitude = 0;
    last_duration = 0;
    last_haptic_hand = 0;
    ASSERT_TRUE(actions.Create(Handle<void *>(1)).ok());
    ASSERT_TRUE(
        actions.Attach(Handle<void *>(1), Handle<void *>(2), Handle<void *>(3))
            .ok());
  }
  bool Sync(uint64_t frame = 1) {
    return actions.Sync(Handle<void *>(2), 123456, frame, 0);
  }
  ControllerDelivery Pop() {
    ControllerDelivery d;
    EXPECT_TRUE(actions.TakeDelivery(&d));
    return d;
  }
};

TEST_F(XrActionsProduction, BindsTouchProAndPlusAndOmitsQuest2Thumbrest) {
  for (const char* name :
       {"/interaction_profiles/facebook/touch_controller_pro",
        "/interaction_profiles/meta/touch_controller_plus",
        "/interaction_profiles/meta/touch_pro_controller",
        "/interaction_profiles/meta/touch_plus_controller"}) {
    const auto& bindings = suggested_paths.at(paths.at(name));
    for (const char* component :
         {"/user/hand/left/input/grip/pose", "/user/hand/right/input/aim/pose",
          "/user/hand/left/input/trigger/value",
          "/user/hand/right/input/squeeze/value",
          "/user/hand/left/output/haptic", "/user/hand/right/output/haptic"}) {
      EXPECT_NE(
          std::find(bindings.begin(), bindings.end(), paths.at(component)),
          bindings.end())
          << name << " " << component;
    }
  }
  const auto& quest = suggested_paths.at(
      paths.at("/interaction_profiles/meta/touch_controller_quest_2"));
  for (const char* component : {"/user/hand/left/input/thumbrest/touch",
                                "/user/hand/right/input/thumbrest/touch"}) {
    EXPECT_EQ(std::find(quest.begin(), quest.end(), paths.at(component)),
              quest.end());
  }
}

TEST_F(XrActionsProduction, HapticSamplesKeepHandLeaseAndClampAmplitude) {
  EXPECT_EQ(simple_haptic_bindings, 2);
  actions.RequestHaptics(1, 1.5f, 50000000, 0);
  ASSERT_TRUE(Sync());
  EXPECT_EQ(vibrations, 1);
  EXPECT_FLOAT_EQ(last_amplitude, 1);
  EXPECT_EQ(last_duration, 50000000);
  EXPECT_EQ(last_haptic_hand, paths["/user/hand/right"]);
  actions.StopHaptics(1);
  ASSERT_TRUE(Sync(2));
  EXPECT_EQ(stops, 1);
}
TEST_F(XrActionsProduction,
       CapacitiveContactDoesNotPressTriggerAndResetsOnFocusLoss) {
  touching = true;
  ASSERT_TRUE(Sync());
  const auto index = static_cast<std::size_t>(ControllerTouch::kTrigger);
  EXPECT_TRUE(actions.snapshot().hands[0].touches[index]);
  EXPECT_TRUE(actions.snapshot().hands[0].touch_active[index]);
  EXPECT_FALSE(actions.snapshot().hands[0].buttons[static_cast<std::size_t>(
      ControllerButton::kTrigger)]);
  focused = false;
  EXPECT_FALSE(Sync(2));
  EXPECT_FALSE(actions.snapshot().hands[0].touches[index]);
  EXPECT_FALSE(actions.snapshot().hands[0].touch_active[index]);
}
TEST_F(XrActionsProduction, ActivatesSetAndDeliversGripTriggerAndPose) {
  trigger = .9f;
  squeeze = .9f;
  stick_y = 1.f;
  ASSERT_TRUE(Sync());
  auto d = Pop();
  EXPECT_EQ(syncs, 1);
  EXPECT_TRUE(
      d.button_levels[0][static_cast<size_t>(ControllerButton::kSqueeze)]);
  EXPECT_TRUE(
      d.button_levels[1][static_cast<size_t>(ControllerButton::kSqueeze)]);
  EXPECT_FLOAT_EQ(d.axes[4], .9f);
  EXPECT_FLOAT_EQ(d.axes[1], -1.f);
  EXPECT_TRUE(actions.hand(0).grip_valid);
  EXPECT_TRUE(actions.hand(1).aim_valid);
  EXPECT_EQ(located_time, 123456);
  squeeze = .65f;
  ASSERT_TRUE(Sync(2));
  EXPECT_TRUE(Pop().button_levels[0][1]);
  squeeze = .5f;
  ASSERT_TRUE(Sync(3));
  EXPECT_FALSE(Pop().button_levels[0][1]);
}
TEST_F(XrActionsProduction,
       InactiveActionsReleaseEvenWithProfileStillAssigned) {
  trigger = squeeze = 1;
  ASSERT_TRUE(Sync());
  Pop();
  input_active = false;
  ASSERT_TRUE(Sync(2));
  auto d = Pop();
  EXPECT_EQ(d.axes[4], 0);
  EXPECT_FALSE(d.button_levels[0][1]);
  EXPECT_FALSE(actions.hand(0).grip_valid);
}
TEST_F(XrActionsProduction, FocusLossDisconnectsAndStopsHaptics) {
  squeeze = 1;
  ASSERT_TRUE(Sync());
  Pop();
  actions.RequestHaptics(0, 1, 100, 0);
  focused = false;
  EXPECT_FALSE(Sync(2));
  auto d = Pop();
  EXPECT_FALSE(d.connected[0]);
  EXPECT_TRUE(d.resync);
  EXPECT_GT(stops, 0);
  EXPECT_FALSE(actions.hand(0).grip_valid);
  focused = true;
  ASSERT_TRUE(Sync(3));
  EXPECT_EQ(vibrations, 0);
}
TEST_F(XrActionsProduction, ResetSurvivesDetachAndNewSessionHasFreshState) {
  squeeze = 1;
  ASSERT_TRUE(Sync());
  auto old = Pop();
  actions.RequestHaptics(0, 1, 100, 0);
  actions.Detach();
  auto reset = Pop();
  EXPECT_FALSE(reset.connected[0]);
  ASSERT_TRUE(
      actions.Attach(Handle<void *>(1), Handle<void *>(4), Handle<void *>(5))
          .ok());
  squeeze = .65f;
  ASSERT_TRUE(Sync(2));
  auto fresh = Pop();
  EXPECT_GT(fresh.session_generation, old.session_generation);
  EXPECT_FALSE(fresh.button_levels[0][1]);
  EXPECT_EQ(vibrations, 0);
}
TEST_F(XrActionsProduction, SimpleSelectBecomesAnalogTrigger) {
  simple = click = true;
  ASSERT_TRUE(Sync());
  EXPECT_FLOAT_EQ(Pop().axes[4], 1.f);
  click = false;
  ASSERT_TRUE(Sync(2));
  EXPECT_FLOAT_EQ(Pop().axes[4], 0.f);
}
TEST_F(XrActionsProduction, QueuedLevelsBelongToTheirOwnFrames) {
  squeeze = 1;
  ASSERT_TRUE(Sync(1));
  squeeze = 0;
  ASSERT_TRUE(Sync(2));
  auto first = Pop(), second = Pop();
  EXPECT_EQ(first.frame, 1u);
  EXPECT_TRUE(first.button_levels[0][1]);
  EXPECT_EQ(second.frame, 2u);
  EXPECT_FALSE(second.button_levels[0][1]);
}
XrActions *delivery_source;
int32_t DrainActions(MocktailVrControllerDelivery *out) {
  ControllerDelivery d;
  if (!delivery_source->TakeDelivery(&d))
    return 0;
  EncodeControllerDelivery(d, out);
  return 1;
}
TEST_F(XrActionsProduction, RoutesActiveInputsThroughAbiAndNativeRouter) {
  using namespace mocktail;
  using namespace mocktail::runtime;
  struct SinkState {
    std::map<int, bool> buttons;
    std::map<int, float> axes;
    int connects = 0, disconnects = 0;
  } state;
  RobloxInputSink sink;
  sink.gamepad = {
      &state,
      [](void *, int32_t, int32_t, bool, int32_t) { return Status::Ok(); },
      [](void *, int32_t, int32_t, int32_t, bool, int32_t) {
        return Status::Ok();
      },
      [](void *p, int32_t, int32_t) {
        ++static_cast<SinkState *>(p)->connects;
        return Status::Ok();
      },
      [](void *p, int32_t) {
        ++static_cast<SinkState *>(p)->disconnects;
        return Status::Ok();
      },
      [](void *p, int32_t, int32_t key, bool pressed) {
        static_cast<SinkState *>(p)->buttons[key] = pressed;
        return Status::Ok();
      },
      [](void *p, int32_t, int32_t axis, float x, float y, float z) {
        static_cast<SinkState *>(p)->axes[axis] =
            axis == 0 ? y : (axis >= 17 ? z : x);
        return Status::Ok();
      }};
  RobloxInputRouter router(sink);
  ASSERT_TRUE(router.Activate({800, 600, 800, 600, 1}, true).ok());
  RobloxXrControllerInput input;
  delivery_source = &actions;
  ASSERT_TRUE(input.InitializeWithAbi(
      [](void *p, const platform::PlatformEvent &e) {
        EXPECT_TRUE(
            static_cast<RobloxInputRouter *>(p)->HandleEvent(e).status.ok());
      },
      &router, DrainActions, nullptr, nullptr));
  squeeze = trigger = stick_y = 1;
  ASSERT_TRUE(Sync());
  input.Drain();
  EXPECT_EQ(state.connects, 1);
  EXPECT_TRUE(state.buttons[102]);
  EXPECT_TRUE(state.buttons[103]);
  EXPECT_GT(state.axes[17], .99f);
  EXPECT_GT(state.axes[0], .99f);
  focused = false;
  EXPECT_FALSE(Sync(2));
  input.Drain();
  EXPECT_EQ(state.disconnects, 1);
  EXPECT_FALSE(state.buttons[102]);
  EXPECT_FALSE(state.buttons[103]);
  input.Shutdown();
}

} // namespace
