#include "mocktail/vr/xr_actions.h"

#include <openxr/openxr.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <utility>
#include <vector>

namespace mocktail::vr {
namespace {

void Log(const char *format, ...) {
  va_list arguments;
  va_start(arguments, format);
  std::vfprintf(stderr, format, arguments);
  va_end(arguments);
  std::fflush(stderr);
}

template <typename T> T XrInfo(XrStructureType type) {
  T value{};
  value.type = type;
  return value;
}

std::string XrResultText(XrInstance instance, XrResult result) {
  char text[XR_MAX_RESULT_STRING_SIZE]{};
  if (instance != XR_NULL_HANDLE) {
    (void)xrResultToString(instance, result, text);
  }
  if (!text[0]) {
    return std::to_string(static_cast<int>(result));
  }
  return text;
}

void CheckXr(XrInstance instance, XrResult result, const char *operation) {
  if (XR_SUCCEEDED(result)) {
    return;
  }
  throw std::runtime_error(std::string(operation) +
                           " failed: " + XrResultText(instance, result));
}

// Registry-verified interaction profiles
// (docs/vr-agent-evidence/controller-2998/openxr-interaction-profiles.md).
constexpr const char *kProfileTouch =
    "/interaction_profiles/oculus/touch_controller";
constexpr const char *kProfileQuest2 =
    "/interaction_profiles/meta/touch_controller_quest_2";
constexpr const char* kTouchProfiles[] = {
    kProfileTouch,
    kProfileQuest2,
    "/interaction_profiles/facebook/touch_controller_pro",
    "/interaction_profiles/meta/touch_controller_plus",
    "/interaction_profiles/meta/touch_pro_controller",
    "/interaction_profiles/meta/touch_plus_controller",
};
constexpr const char *kProfileIndex =
    "/interaction_profiles/valve/index_controller";
constexpr const char *kProfileVive =
    "/interaction_profiles/htc/vive_controller";
constexpr const char *kProfileSimple =
    "/interaction_profiles/khr/simple_controller";

} // namespace

// All XR objects of one XrActions instance.
struct XrActions::Impl {
  XrInstance instance = XR_NULL_HANDLE;
  XrSession session = XR_NULL_HANDLE;
  XrSpace local_space = XR_NULL_HANDLE; // borrowed from the backend
  XrActionSet set = XR_NULL_HANDLE;
  XrAction grip[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
  XrAction aim[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
  XrSpace grip_space[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
  XrSpace aim_space[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
  XrAction touches[static_cast<std::size_t>(ControllerTouch::kCount)] = {};
  XrAction trigger = XR_NULL_HANDLE;
  XrAction squeeze = XR_NULL_HANDLE;
  XrAction stick = XR_NULL_HANDLE;
  XrAction trigger_click = XR_NULL_HANDLE;
  XrAction squeeze_click = XR_NULL_HANDLE;
  XrAction stick_click = XR_NULL_HANDLE;
  XrAction face_a = XR_NULL_HANDLE;
  XrAction face_b = XR_NULL_HANDLE;
  XrAction face_x = XR_NULL_HANDLE;
  XrAction face_y = XR_NULL_HANDLE;
  XrAction menu = XR_NULL_HANDLE;
  XrAction haptic = XR_NULL_HANDLE;
  XrPath subaction_left = XR_NULL_PATH;
  XrPath subaction_right = XR_NULL_PATH;

  std::mutex haptic_mutex;
  struct HapticRequest {
    bool pending = false;
    bool stop = false;
    float amplitude = 0.f;
    std::uint64_t duration_ns = 0;
    float frequency_hz = 0.f;
  };
  HapticRequest haptics[2];

  XrPath active_profile[2] = {XR_NULL_PATH, XR_NULL_PATH};
  std::string active_profile_name[2];
  // Trigger-click actions that reported active this frame, per hand: the
  // analog-derived click must not override a native click action.
  bool native_trigger_click[2] = {false, false};
};

XrActions::~XrActions() { Destroy(); }

Status XrActions::Create(void *xr_instance) {
  if (created_) {
    return Status::Ok();
  }
  if (xr_instance == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "XR actions require an instance");
  }
  const auto instance = static_cast<XrInstance>(xr_instance);
  delete impl_;
  impl_ = new Impl();
  impl_->instance = instance;
  try {
    auto set_info =
        XrInfo<XrActionSetCreateInfo>(XR_TYPE_ACTION_SET_CREATE_INFO);
    std::strncpy(set_info.actionSetName, "mocktail_vr",
                 XR_MAX_ACTION_SET_NAME_SIZE - 1);
    std::strncpy(set_info.localizedActionSetName, "Mocktail VR controls",
                 XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    set_info.priority = 0;
    CheckXr(instance, xrCreateActionSet(instance, &set_info, &impl_->set),
            "xrCreateActionSet");
    CheckXr(instance,
            xrStringToPath(instance, "/user/hand/left", &impl_->subaction_left),
            "xrStringToPath left");
    CheckXr(
        instance,
        xrStringToPath(instance, "/user/hand/right", &impl_->subaction_right),
        "xrStringToPath right");
    const std::array<XrPath, 2> subactions{impl_->subaction_left,
                                           impl_->subaction_right};

    auto make = [&](XrActionType type, const char *name) {
      auto info = XrInfo<XrActionCreateInfo>(XR_TYPE_ACTION_CREATE_INFO);
      info.actionType = type;
      info.countSubactionPaths = 2;
      info.subactionPaths = subactions.data();
      std::strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
      // A non-empty localized name is mandatory; runtimes reject an empty one
      // with XR_ERROR_LOCALIZED_NAME_INVALID.
      std::strncpy(info.localizedActionName, name,
                   XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
      XrAction action = XR_NULL_HANDLE;
      CheckXr(instance, xrCreateAction(impl_->set, &info, &action), name);
      return action;
    };

    impl_->grip[0] = make(XR_ACTION_TYPE_POSE_INPUT, "grip_left");
    impl_->grip[1] = make(XR_ACTION_TYPE_POSE_INPUT, "grip_right");
    impl_->aim[0] = make(XR_ACTION_TYPE_POSE_INPUT, "aim_left");
    impl_->aim[1] = make(XR_ACTION_TYPE_POSE_INPUT, "aim_right");
    impl_->trigger = make(XR_ACTION_TYPE_FLOAT_INPUT, "trigger");
    impl_->squeeze = make(XR_ACTION_TYPE_FLOAT_INPUT, "squeeze");
    impl_->stick = make(XR_ACTION_TYPE_VECTOR2F_INPUT, "thumbstick");
    impl_->trigger_click = make(XR_ACTION_TYPE_BOOLEAN_INPUT, "trigger_click");
    impl_->squeeze_click = make(XR_ACTION_TYPE_BOOLEAN_INPUT, "squeeze_click");
    impl_->stick_click = make(XR_ACTION_TYPE_BOOLEAN_INPUT, "thumbstick_click");
    impl_->face_a = make(XR_ACTION_TYPE_BOOLEAN_INPUT, "face_a");
    impl_->face_b = make(XR_ACTION_TYPE_BOOLEAN_INPUT, "face_b");
    impl_->face_x = make(XR_ACTION_TYPE_BOOLEAN_INPUT, "face_x");
    impl_->face_y = make(XR_ACTION_TYPE_BOOLEAN_INPUT, "face_y");
    impl_->menu = make(XR_ACTION_TYPE_BOOLEAN_INPUT, "menu");
    const char *touch_names[] = {
        "trigger_touch", "thumbstick_touch", "thumbrest_touch", "a_touch",
        "b_touch",       "x_touch",          "y_touch"};
    for (std::size_t i = 0; i < std::size(touch_names); ++i)
      impl_->touches[i] = make(XR_ACTION_TYPE_BOOLEAN_INPUT, touch_names[i]);
    impl_->haptic = make(XR_ACTION_TYPE_VIBRATION_OUTPUT, "haptic");

    auto path = [&](const char *text) {
      XrPath result = XR_NULL_PATH;
      CheckXr(instance, xrStringToPath(instance, text, &result), text);
      return result;
    };
    // Each profile is suggested independently: a runtime that does not know an
    // optional profile rejects only that suggestion, never the primary Touch
    // binding. The system-reserved button is not bound on any profile.
    auto suggest = [&](const char *profile,
                       std::vector<std::pair<XrAction, XrPath>> binds) {
      auto touch = [&](ControllerTouch kind, const char *component) {
        binds.emplace_back(impl_->touches[static_cast<std::size_t>(kind)],
                           path(component));
      };
      const bool is_touch = std::any_of(
          std::begin(kTouchProfiles), std::end(kTouchProfiles),
          [&](const char* name) { return std::strcmp(profile, name) == 0; });
      const bool is_index = std::strcmp(profile, kProfileIndex) == 0;
      if (is_touch || is_index) {
        touch(ControllerTouch::kTrigger, "/user/hand/left/input/trigger/touch");
        touch(ControllerTouch::kTrigger,
              "/user/hand/right/input/trigger/touch");
        touch(ControllerTouch::kThumbstick,
              "/user/hand/left/input/thumbstick/touch");
        touch(ControllerTouch::kThumbstick,
              "/user/hand/right/input/thumbstick/touch");
        touch(ControllerTouch::kA, "/user/hand/right/input/a/touch");
        touch(ControllerTouch::kB, "/user/hand/right/input/b/touch");
        touch(ControllerTouch::kX, is_touch ? "/user/hand/left/input/x/touch"
                                            : "/user/hand/left/input/a/touch");
        touch(ControllerTouch::kY, is_touch ? "/user/hand/left/input/y/touch"
                                            : "/user/hand/left/input/b/touch");
      }
      if (is_touch && std::strcmp(profile, kProfileQuest2) != 0) {
        touch(ControllerTouch::kThumbrest,
              "/user/hand/left/input/thumbrest/touch");
        touch(ControllerTouch::kThumbrest,
              "/user/hand/right/input/thumbrest/touch");
      }
      std::vector<XrActionSuggestedBinding> suggested;
      suggested.reserve(binds.size());
      for (const auto &bind : binds) {
        XrActionSuggestedBinding entry{};
        entry.action = bind.first;
        entry.binding = bind.second;
        suggested.push_back(entry);
      }
      auto info = XrInfo<XrInteractionProfileSuggestedBinding>(
          XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING);
      info.interactionProfile = path(profile);
      info.countSuggestedBindings =
          static_cast<std::uint32_t>(suggested.size());
      info.suggestedBindings = suggested.data();
      const XrResult result =
          xrSuggestInteractionProfileBindings(instance, &info);
      if (XR_FAILED(result)) {
        Log("  [vr-input] bindings for %s not accepted (%s)\n", profile,
            XrResultText(instance, result).c_str());
      }
    };

    const XrPath l_grip = path("/user/hand/left/input/grip/pose");
    const XrPath r_grip = path("/user/hand/right/input/grip/pose");
    const XrPath l_aim = path("/user/hand/left/input/aim/pose");
    const XrPath r_aim = path("/user/hand/right/input/aim/pose");
    const XrPath l_haptic = path("/user/hand/left/output/haptic");
    const XrPath r_haptic = path("/user/hand/right/output/haptic");

    for (const char* profile : kTouchProfiles) {
      suggest(
          profile,
          {
              {impl_->grip[0], l_grip},
              {impl_->grip[1], r_grip},
              {impl_->aim[0], l_aim},
              {impl_->aim[1], r_aim},
              {impl_->trigger, path("/user/hand/left/input/trigger/value")},
              {impl_->trigger, path("/user/hand/right/input/trigger/value")},
              {impl_->squeeze, path("/user/hand/left/input/squeeze/value")},
              {impl_->squeeze, path("/user/hand/right/input/squeeze/value")},
              {impl_->stick, path("/user/hand/left/input/thumbstick")},
              {impl_->stick, path("/user/hand/right/input/thumbstick")},
              {impl_->stick_click,
               path("/user/hand/left/input/thumbstick/click")},
              {impl_->stick_click,
               path("/user/hand/right/input/thumbstick/click")},
              {impl_->face_x, path("/user/hand/left/input/x/click")},
              {impl_->face_y, path("/user/hand/left/input/y/click")},
              {impl_->face_a, path("/user/hand/right/input/a/click")},
              {impl_->face_b, path("/user/hand/right/input/b/click")},
              {impl_->menu, path("/user/hand/left/input/menu/click")},
              {impl_->haptic, l_haptic},
              {impl_->haptic, r_haptic},
          });
    }
    suggest(
        kProfileIndex,
        {
            {impl_->grip[0], l_grip},
            {impl_->grip[1], r_grip},
            {impl_->aim[0], l_aim},
            {impl_->aim[1], r_aim},
            {impl_->trigger, path("/user/hand/left/input/trigger/value")},
            {impl_->trigger, path("/user/hand/right/input/trigger/value")},
            {impl_->trigger_click, path("/user/hand/left/input/trigger/click")},
            {impl_->trigger_click,
             path("/user/hand/right/input/trigger/click")},
            {impl_->squeeze, path("/user/hand/left/input/squeeze/force")},
            {impl_->squeeze, path("/user/hand/right/input/squeeze/force")},
            {impl_->stick, path("/user/hand/left/input/thumbstick")},
            {impl_->stick, path("/user/hand/right/input/thumbstick")},
            {impl_->stick_click,
             path("/user/hand/left/input/thumbstick/click")},
            {impl_->stick_click,
             path("/user/hand/right/input/thumbstick/click")},
            {impl_->face_x, path("/user/hand/left/input/a/click")},
            {impl_->face_a, path("/user/hand/right/input/a/click")},
            {impl_->face_y, path("/user/hand/left/input/b/click")},
            {impl_->face_b, path("/user/hand/right/input/b/click")},
            {impl_->haptic, l_haptic},
            {impl_->haptic, r_haptic},
        });
    suggest(
        kProfileVive,
        {
            {impl_->squeeze_click, path("/user/hand/left/input/squeeze/click")},
            {impl_->squeeze_click,
             path("/user/hand/right/input/squeeze/click")},
            {impl_->grip[0], l_grip},
            {impl_->grip[1], r_grip},
            {impl_->aim[0], l_aim},
            {impl_->aim[1], r_aim},
            {impl_->trigger, path("/user/hand/left/input/trigger/value")},
            {impl_->trigger, path("/user/hand/right/input/trigger/value")},
            {impl_->stick, path("/user/hand/left/input/trackpad")},
            {impl_->stick, path("/user/hand/right/input/trackpad")},
            {impl_->stick_click, path("/user/hand/left/input/trackpad/click")},
            {impl_->stick_click, path("/user/hand/right/input/trackpad/click")},
            {impl_->menu, path("/user/hand/left/input/menu/click")},
            {impl_->menu, path("/user/hand/right/input/menu/click")},
            {impl_->haptic, l_haptic},
            {impl_->haptic, r_haptic},
        });
    suggest(
        kProfileSimple,
        {
            {impl_->grip[0], l_grip},
            {impl_->grip[1], r_grip},
            {impl_->aim[0], l_aim},
            {impl_->aim[1], r_aim},
            {impl_->trigger_click, path("/user/hand/left/input/select/click")},
            {impl_->trigger_click, path("/user/hand/right/input/select/click")},
            {impl_->menu, path("/user/hand/left/input/menu/click")},
            {impl_->menu, path("/user/hand/right/input/menu/click")},
            {impl_->haptic, l_haptic},
            {impl_->haptic, r_haptic},
        });

    created_ = true;
    Log("  [vr-input] action set created: grip+aim poses, trigger/squeeze "
        "analog, thumbstick, face/menu buttons, haptic output; bindings "
        "suggested for Touch/Quest2/Touch Pro/Touch Plus/Index/Vive/Simple "
        "(system button never "
        "bound)\n");
    return Status::Ok();
  } catch (const std::exception &error) {
    Destroy();
    return Status::Error(StatusCode::kUnavailable, error.what());
  }
}

Status XrActions::Attach(void *xr_instance, void *xr_session,
                         void *xr_local_space) {
  if (!created_ || impl_ == nullptr) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "XR actions are not created");
  }
  if (xr_session == nullptr || xr_local_space == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "XR actions attach requires a session and space");
  }
  const auto instance = static_cast<XrInstance>(xr_instance);
  const auto session = static_cast<XrSession>(xr_session);
  try {
    auto attach_info = XrInfo<XrSessionActionSetsAttachInfo>(
        XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO);
    attach_info.countActionSets = 1;
    attach_info.actionSets = &impl_->set;
    CheckXr(instance, xrAttachSessionActionSets(session, &attach_info),
            "xrAttachSessionActionSets");

    for (int hand = 0; hand < 2; ++hand) {
      const XrPath subaction =
          hand == 0 ? impl_->subaction_left : impl_->subaction_right;
      auto space_info =
          XrInfo<XrActionSpaceCreateInfo>(XR_TYPE_ACTION_SPACE_CREATE_INFO);
      space_info.subactionPath = subaction;
      space_info.poseInActionSpace.orientation.w = 1.f;
      space_info.action = impl_->grip[hand];
      CheckXr(
          instance,
          xrCreateActionSpace(session, &space_info, &impl_->grip_space[hand]),
          "xrCreateActionSpace grip");
      space_info.action = impl_->aim[hand];
      CheckXr(
          instance,
          xrCreateActionSpace(session, &space_info, &impl_->aim_space[hand]),
          "xrCreateActionSpace aim");
    }
    impl_->session = session;
    impl_->local_space = static_cast<XrSpace>(xr_local_space);
    attached_ = true;
    session_generation_++;
    Log("  [vr-input] actions attached to session (generation %llu)\n",
        static_cast<unsigned long long>(session_generation_));
    return Status::Ok();
  } catch (const std::exception &error) {
    Detach();
    return Status::Error(StatusCode::kUnavailable, error.what());
  }
}

void XrActions::ResetInput() {
  const bool had_input =
      snapshot_.hands[0].connected || snapshot_.hands[1].connected;
  const auto frame = snapshot_.frame;
  builder_ = ControllerStateBuilder{};
  snapshot_ = {};
  hands_[0] = {};
  hands_[1] = {};
  if (had_input) {
    delivery_queue_.Clear();
    ControllerDelivery reset;
    reset.frame = frame;
    reset.session_generation = session_generation_;
    reset.resync = true;
    delivery_queue_.Push(reset);
  }
  last_delivered_connected_[0] = last_delivered_connected_[1] = false;
  if (impl_) {
    std::lock_guard<std::mutex> lock(impl_->haptic_mutex);
    for (int hand = 0; hand < 2; ++hand) {
      impl_->haptics[hand] = {};
      if (attached_ && impl_->session) {
        auto stop = XrInfo<XrHapticActionInfo>(XR_TYPE_HAPTIC_ACTION_INFO);
        stop.action = impl_->haptic;
        stop.subactionPath =
            hand ? impl_->subaction_right : impl_->subaction_left;
        (void)xrStopHapticFeedback(impl_->session, &stop);
      }
    }
  }
}

void XrActions::Detach() {
  ResetInput();
  if (impl_ == nullptr) {
    attached_ = false;
    return;
  }
  // Haptics must not outlive the session.
  if (attached_ && impl_->session != XR_NULL_HANDLE) {
    for (int hand = 0; hand < 2; ++hand) {
      auto stop = XrInfo<XrHapticActionInfo>(XR_TYPE_HAPTIC_ACTION_INFO);
      stop.action = impl_->haptic;
      stop.subactionPath =
          hand == 0 ? impl_->subaction_left : impl_->subaction_right;
      (void)xrStopHapticFeedback(impl_->session, &stop);
    }
  }
  for (int hand = 0; hand < 2; ++hand) {
    if (impl_->grip_space[hand] != XR_NULL_HANDLE) {
      (void)xrDestroySpace(impl_->grip_space[hand]);
      impl_->grip_space[hand] = XR_NULL_HANDLE;
    }
    if (impl_->aim_space[hand] != XR_NULL_HANDLE) {
      (void)xrDestroySpace(impl_->aim_space[hand]);
      impl_->aim_space[hand] = XR_NULL_HANDLE;
    }
    impl_->active_profile[hand] = XR_NULL_PATH;
    impl_->active_profile_name[hand].clear();
    impl_->native_trigger_click[hand] = false;
  }
  impl_->session = XR_NULL_HANDLE;
  impl_->local_space = XR_NULL_HANDLE;
  attached_ = false;
  hands_[0] = VrHandPose{};
  hands_[1] = VrHandPose{};
  // Queued deliveries belong to the detached session; a replacement session
  // must start from a clean slate with fresh connection levels.
  // Preserve queued reset until the input thread drains it.
  last_delivered_connected_[0] = false;
  last_delivered_connected_[1] = false;
}

void XrActions::Destroy() {
  Detach();
  if (impl_ != nullptr) {
    const auto instance = impl_->instance;
    if (instance != XR_NULL_HANDLE) {
      const std::array<XrAction, 16> actions{
          impl_->grip[0],     impl_->grip[1],       impl_->aim[0],
          impl_->aim[1],      impl_->trigger,       impl_->squeeze,
          impl_->stick,       impl_->trigger_click, impl_->squeeze_click,
          impl_->stick_click, impl_->face_a,        impl_->face_b,
          impl_->face_x,      impl_->face_y,        impl_->menu,
          impl_->haptic};
      for (const XrAction action : actions) {
        if (action != XR_NULL_HANDLE) {
          (void)xrDestroyAction(action);
        }
      }
      for (const auto action : impl_->touches)
        if (action != XR_NULL_HANDLE)
          (void)xrDestroyAction(action);
      if (impl_->set != XR_NULL_HANDLE) {
        (void)xrDestroyActionSet(impl_->set);
      }
    }
    delete impl_;
    impl_ = nullptr;
  }
  created_ = false;
  attached_ = false;
  snapshot_ = ControllerSnapshot{};
  hands_[0] = VrHandPose{};
  hands_[1] = VrHandPose{};
  // Preserve queued reset until the input thread drains it.
  last_delivered_connected_[0] = false;
  last_delivered_connected_[1] = false;
  session_generation_ = 0;
}

void XrActions::RequestHaptics(int hand, float amplitude,
                               std::uint64_t duration_ns, float frequency_hz) {
  if (impl_ == nullptr || !attached_ || (hand != 0 && hand != 1)) {
    return;
  }
  std::lock_guard<std::mutex> lock(impl_->haptic_mutex);
  auto &request = impl_->haptics[hand];
  request = {};
  request.pending = true;
  request.stop = false;
  request.amplitude =
      std::clamp(std::isfinite(amplitude) ? amplitude : 0.f, 0.f, 1.f);
  request.duration_ns = duration_ns;
  request.frequency_hz =
      std::isfinite(frequency_hz) ? std::max(0.f, frequency_hz) : 0.f;
}

void XrActions::StopHaptics(int hand) {
  if (impl_ == nullptr || !attached_ || (hand != 0 && hand != 1)) {
    return;
  }
  std::lock_guard<std::mutex> lock(impl_->haptic_mutex);
  impl_->haptics[hand] = Impl::HapticRequest{};
  impl_->haptics[hand].pending = true;
  impl_->haptics[hand].stop = true;
}

void XrActions::NoteProfileChanged() {
  if (impl_ == nullptr) {
    return;
  }
  for (int hand = 0; hand < 2; ++hand) {
    impl_->active_profile[hand] = XR_NULL_PATH;
    impl_->active_profile_name[hand].clear();
  }
}

std::string XrActions::ActiveProfile(int hand) const {
  if (impl_ == nullptr || (hand != 0 && hand != 1)) {
    return {};
  }
  return impl_->active_profile_name[hand];
}

bool XrActions::TakeDelivery(ControllerDelivery *out) {
  return delivery_queue_.Pop(out);
}

bool XrActions::Sync(void *xr_session, std::uint64_t predicted_display_time,
                     std::uint64_t frame, std::uint64_t /*generation*/) {
  if (!attached_ || impl_ == nullptr) {
    return false;
  }
  const auto session = static_cast<XrSession>(xr_session);
  const auto instance = impl_->instance;
  const auto time = static_cast<XrTime>(predicted_display_time);
  try {
    auto sync_info = XrInfo<XrActionsSyncInfo>(XR_TYPE_ACTIONS_SYNC_INFO);
    const XrActiveActionSet active{impl_->set, XR_NULL_PATH};
    sync_info.countActiveActionSets = 1;
    sync_info.activeActionSets = &active;
    const XrResult sync_result = xrSyncActions(session, &sync_info);
    if (sync_result == XR_SESSION_NOT_FOCUSED) {
      // Normal while the system compositor owns input: no fresh state exists
      // this frame and nothing may be delivered.
      ResetInput();
      return false;
    }
    if (XR_FAILED(sync_result)) {
      Log("  [vr-input] xrSyncActions failed: %s\n",
          XrResultText(instance, sync_result).c_str());
      ResetInput();
      return false;
    }

    for (int hand = 0; hand < 2; ++hand) {
      const XrPath subaction =
          hand == 0 ? impl_->subaction_left : impl_->subaction_right;

      // Inactive actions must neutralize previous state, even if a profile
      // remains assigned after the device sleeps or the runtime changes
      // bindings.
      for (size_t i = 0; i < static_cast<size_t>(ControllerButton::kCount); ++i)
        builder_.SetButton(hand, static_cast<ControllerButton>(i), false, true);
      for (size_t i = 0; i < static_cast<size_t>(ControllerAxis::kCount); ++i)
        builder_.SetAxis(hand, static_cast<ControllerAxis>(i), 0.f, true);
      for (std::size_t i = 0;
           i < static_cast<std::size_t>(ControllerTouch::kCount); ++i)
        builder_.SetTouch(hand, static_cast<ControllerTouch>(i), false, false);
      // Connection state comes from the runtime's current profile for the
      // hand; profile changes refresh diagnostics here and on the event.
      auto profile_info =
          XrInfo<XrInteractionProfileState>(XR_TYPE_INTERACTION_PROFILE_STATE);
      bool connected = false;
      if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(session, subaction,
                                                      &profile_info)) &&
          profile_info.interactionProfile != XR_NULL_PATH) {
        connected = true;
        if (profile_info.interactionProfile != impl_->active_profile[hand]) {
          impl_->active_profile[hand] = profile_info.interactionProfile;
          char buffer[XR_MAX_PATH_LENGTH]{};
          std::uint32_t length = 0;
          if (XR_SUCCEEDED(xrPathToString(instance,
                                          profile_info.interactionProfile,
                                          sizeof(buffer), &length, buffer))) {
            impl_->active_profile_name[hand] = buffer;
          }
          builder_.SetProfile(hand, impl_->active_profile_name[hand]);
          Log("  [vr-input] hand %d interaction profile: %s\n", hand,
              impl_->active_profile_name[hand].c_str());
        }
      } else {
        if (impl_->active_profile[hand] != XR_NULL_PATH) {
          Log("  [vr-input] hand %d controller disconnected\n", hand);
        }
        impl_->active_profile[hand] = XR_NULL_PATH;
        impl_->active_profile_name[hand].clear();
        builder_.SetProfile(hand, {});
      }
      if (connected && builder_.snapshot().hands[hand].profile !=
                           impl_->active_profile_name[hand])
        builder_.SetProfile(hand, impl_->active_profile_name[hand]);
      builder_.SetConnected(hand, connected);
      if (!connected) {
        // A disconnected hand must release anything held: force every button
        // level to false so Publish emits at most one release edge per button
        // and nothing can stay stuck down.
        for (std::size_t index = 0;
             index < static_cast<std::size_t>(ControllerButton::kCount);
             ++index) {
          builder_.SetButton(hand, static_cast<ControllerButton>(index), false,
                             true);
        }
        builder_.SetAxis(hand, ControllerAxis::kTrigger, 0.f, true);
        builder_.SetAxis(hand, ControllerAxis::kSqueeze, 0.f, true);
        builder_.SetAxis(hand, ControllerAxis::kThumbstickX, 0.f, true);
        builder_.SetAxis(hand, ControllerAxis::kThumbstickY, 0.f, true);
        builder_.SetPoseValid(hand, false);
        hands_[hand] = VrHandPose{};
        continue;
      }

      // Grip and aim poses located at the very predicted display time the head
      // and eyes used for this frame.
      VrHandPose hand_pose;
      bool hand_active = false;
      for (int which = 0; which < 2; ++which) {
        const XrSpace space =
            which == 0 ? impl_->grip_space[hand] : impl_->aim_space[hand];
        XrSpaceLocation location =
            XrInfo<XrSpaceLocation>(XR_TYPE_SPACE_LOCATION);
        const XrResult located =
            xrLocateSpace(space, impl_->local_space, time, &location);
        const bool orientation_valid =
            XR_SUCCEEDED(located) &&
            (location.locationFlags &
             XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
        const bool position_valid = XR_SUCCEEDED(located) &&
                                    (location.locationFlags &
                                     XR_SPACE_LOCATION_POSITION_VALID_BIT) != 0;
        // The guest record has a single valid gate and the consumer converts
        // position+quaternion together: orientation-only tracking is therefore
        // reported untracked rather than paired with a stale position.
        auto pose_info =
            XrInfo<XrActionStateGetInfo>(XR_TYPE_ACTION_STATE_GET_INFO);
        pose_info.action = which == 0 ? impl_->grip[hand] : impl_->aim[hand];
        pose_info.subactionPath = subaction;
        auto pose_state = XrInfo<XrActionStatePose>(XR_TYPE_ACTION_STATE_POSE);
        const bool active = XR_SUCCEEDED(xrGetActionStatePose(
                                session, &pose_info, &pose_state)) &&
                            pose_state.isActive;
        hand_active |= active;
        const bool usable = active && orientation_valid && position_valid;
        float *position =
            which == 0 ? hand_pose.grip_position : hand_pose.aim_position;
        float *orientation =
            which == 0 ? hand_pose.grip_orientation : hand_pose.aim_orientation;
        if (usable) {
          position[0] = location.pose.position.x;
          position[1] = location.pose.position.y;
          position[2] = location.pose.position.z;
          orientation[0] = location.pose.orientation.x;
          orientation[1] = location.pose.orientation.y;
          orientation[2] = location.pose.orientation.z;
          orientation[3] = location.pose.orientation.w;
        }
        if (which == 0) {
          hand_pose.grip_valid = usable;
        } else {
          hand_pose.aim_valid = usable;
        }
      }
      hands_[hand] = hand_pose;
      builder_.SetPoseValid(hand, hand_pose.grip_valid);

      // Analog inputs. XrActionState{Float,Vector2f} carry the value in
      // currentState and validity in isActive.
      auto get_info =
          XrInfo<XrActionStateGetInfo>(XR_TYPE_ACTION_STATE_GET_INFO);
      get_info.subactionPath = subaction;

      get_info.action = impl_->trigger;
      XrActionStateFloat trigger_state =
          XrInfo<XrActionStateFloat>(XR_TYPE_ACTION_STATE_FLOAT);
      if (XR_SUCCEEDED(
              xrGetActionStateFloat(session, &get_info, &trigger_state)) &&
          trigger_state.isActive) {
        hand_active = true;
        builder_.SetAxis(hand, ControllerAxis::kTrigger,
                         trigger_state.currentState, true);
      }
      get_info.action = impl_->squeeze;
      XrActionStateFloat squeeze_state =
          XrInfo<XrActionStateFloat>(XR_TYPE_ACTION_STATE_FLOAT);
      if (XR_SUCCEEDED(
              xrGetActionStateFloat(session, &get_info, &squeeze_state)) &&
          squeeze_state.isActive) {
        hand_active = true;
        builder_.SetAxis(hand, ControllerAxis::kSqueeze,
                         squeeze_state.currentState, true);
      }
      get_info.action = impl_->stick;
      XrActionStateVector2f stick_state =
          XrInfo<XrActionStateVector2f>(XR_TYPE_ACTION_STATE_VECTOR2F);
      if (XR_SUCCEEDED(
              xrGetActionStateVector2f(session, &get_info, &stick_state)) &&
          stick_state.isActive) {
        hand_active = true;
        builder_.SetAxis(hand, ControllerAxis::kThumbstickX,
                         stick_state.currentState.x, true);
        builder_.SetAxis(hand, ControllerAxis::kThumbstickY,
                         stick_state.currentState.y, true);
      }

      for (std::size_t i = 0;
           i < static_cast<std::size_t>(ControllerTouch::kCount); ++i) {
        get_info.action = impl_->touches[i];
        auto state = XrInfo<XrActionStateBoolean>(XR_TYPE_ACTION_STATE_BOOLEAN);
        const bool available =
            XR_SUCCEEDED(xrGetActionStateBoolean(session, &get_info, &state)) &&
            state.isActive;
        builder_.SetTouch(hand, static_cast<ControllerTouch>(i),
                          state.currentState, available);
      }

      // Boolean inputs.
      const std::pair<XrAction, ControllerButton> booleans[] = {
          {impl_->trigger_click, ControllerButton::kTrigger},
          {impl_->squeeze_click, ControllerButton::kSqueeze},
          {impl_->stick_click, ControllerButton::kThumbstick},
          {impl_->face_a, ControllerButton::kFaceSouth},
          {impl_->face_b, ControllerButton::kFaceEast},
          {impl_->face_x, ControllerButton::kFaceWest},
          {impl_->face_y, ControllerButton::kFaceNorth},
          {impl_->menu, ControllerButton::kMenu},
      };
      bool native_trigger_click = false;
      bool native_squeeze_click = false;
      for (const auto &entry : booleans) {
        get_info.action = entry.first;
        XrActionStateBoolean state =
            XrInfo<XrActionStateBoolean>(XR_TYPE_ACTION_STATE_BOOLEAN);
        if (!XR_SUCCEEDED(
                xrGetActionStateBoolean(session, &get_info, &state)) ||
            !state.isActive) {
          continue;
        }
        hand_active = true;
        const bool pressed = state.currentState != XR_FALSE;
        builder_.SetButton(hand, entry.second, pressed, true);
        if (entry.first == impl_->squeeze_click)
          native_squeeze_click = true;
        if (entry.first == impl_->trigger_click) {
          native_trigger_click = true;
        }
      }
      // Native VR has analog shoulder channels as well as trigger channels.
      // Only click-only profiles (Vive) need a synthesized 0/1 squeeze value.
      if (native_squeeze_click && !squeeze_state.isActive)
        builder_.SetAxis(
            hand, ControllerAxis::kSqueeze,
            builder_.snapshot().hands[hand].buttons[static_cast<std::size_t>(
                ControllerButton::kSqueeze)]
                ? 1.f
                : 0.f,
            true);
      if (native_trigger_click && !trigger_state.isActive)
        builder_.SetAxis(
            hand, ControllerAxis::kTrigger,
            builder_.snapshot()
                    .hands[hand]
                    .buttons[static_cast<size_t>(ControllerButton::kTrigger)]
                ? 1.f
                : 0.f,
            true);
      if (!native_squeeze_click)
        builder_.SetButton(hand, ControllerButton::kSqueeze,
                           builder_.AnalogClick(hand, ControllerAxis::kSqueeze),
                           true);
      builder_.SetConnected(hand, hand_active);
      impl_->native_trigger_click[hand] = native_trigger_click;
      // Profiles without a trigger click action (Touch, Vive) derive the click
      // from the analog value with hysteresis; the native analog value itself
      // is always preserved.
      if (!native_trigger_click) {
        builder_.SetButton(hand, ControllerButton::kTrigger,
                           builder_.AnalogClick(hand, ControllerAxis::kTrigger),
                           true);
      }
    }

    // One coherent snapshot for the frame; edges fire exactly once here.
    std::vector<ControllerStateBuilder::Edge> edges;
    snapshot_ = builder_.Publish(frame, session_generation_, &edges);

    ControllerDelivery delivery;
    delivery.frame = frame;
    delivery.session_generation = session_generation_;
    for (int hand = 0; hand < 2; ++hand) {
      for (size_t i = 0; i < static_cast<size_t>(ControllerButton::kCount); ++i)
        delivery.button_levels[hand][i] = snapshot_.hands[hand].buttons[i];
      delivery.connected[hand] = snapshot_.hands[hand].connected;
      delivery.connection_changed[hand] =
          delivery.connected[hand] != last_delivered_connected_[hand];
      last_delivered_connected_[hand] = delivery.connected[hand];
      delivery.axes_valid[hand] = snapshot_.hands[hand].connected;
      const auto &axes = snapshot_.hands[hand].axes;
      const int stick_x =
          ControllerAxisToSdlOrdinal(hand, ControllerAxis::kThumbstickX);
      const int stick_y =
          ControllerAxisToSdlOrdinal(hand, ControllerAxis::kThumbstickY);
      const int trigger =
          ControllerAxisToSdlOrdinal(hand, ControllerAxis::kTrigger);
      delivery.axes[stick_x] =
          axes[static_cast<std::size_t>(ControllerAxis::kThumbstickX)];
      delivery.axes[stick_y] =
          -axes[static_cast<std::size_t>(ControllerAxis::kThumbstickY)];
      delivery.axes[trigger] =
          axes[static_cast<std::size_t>(ControllerAxis::kTrigger)];
    }
    for (const auto &edge : edges) {
      if (delivery.edges.size() >= ControllerDelivery::kMaxEdges) {
        // Bounded growth: remaining edges are dropped, but a level resync is
        // demanded so the consumer converges and nothing sticks down.
        delivery.resync = true;
        break;
      }
      delivery.edges.push_back(
          ControllerDelivery::ButtonEdge{edge.hand, edge.button, edge.pressed});
    }
    delivery_queue_.Push(std::move(delivery));

    // Apply queued haptic requests inside this XR call window.
    {
      std::lock_guard<std::mutex> lock(impl_->haptic_mutex);
      for (int hand = 0; hand < 2; ++hand) {
        auto &request = impl_->haptics[hand];
        if (!snapshot_.hands[hand].connected) {
          request = {};
          request.pending = true;
          request.stop = true;
        }
        if (!request.pending) {
          continue;
        }
        request.pending = false;
        auto haptic_info =
            XrInfo<XrHapticActionInfo>(XR_TYPE_HAPTIC_ACTION_INFO);
        haptic_info.action = impl_->haptic;
        haptic_info.subactionPath =
            hand == 0 ? impl_->subaction_left : impl_->subaction_right;
        if (request.stop || !snapshot_.hands[hand].connected) {
          // Never vibrate a disconnected controller.
          (void)xrStopHapticFeedback(session, &haptic_info);
          continue;
        }
        auto vibration = XrInfo<XrHapticVibration>(XR_TYPE_HAPTIC_VIBRATION);
        vibration.amplitude = request.amplitude;
        vibration.duration = request.duration_ns == 0
                                 ? XR_MIN_HAPTIC_DURATION
                                 : static_cast<XrDuration>(std::min<uint64_t>(
                                       request.duration_ns, INT64_MAX));
        // 0 lets the runtime choose its resonant frequency; the runtime
        // clamps unsupported frequencies itself.
        vibration.frequency = request.frequency_hz;
        (void)xrApplyHapticFeedback(
            session, &haptic_info,
            reinterpret_cast<const XrHapticBaseHeader *>(&vibration));
      }
    }
    return true;
  } catch (const std::exception &error) {
    Log("  [vr-input] action sync failed: %s\n", error.what());
    ResetInput();
    return false;
  }
}

} // namespace mocktail::vr
