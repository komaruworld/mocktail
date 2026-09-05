#include <SDL3/SDL_gamepad.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <utility>

#include "runtime/roblox_input_router.h"

namespace mocktail {
namespace runtime {
namespace {

static_assert(SDL_GAMEPAD_AXIS_COUNT == 6);
static_assert(SDL_GAMEPAD_BUTTON_COUNT <= 32);

constexpr std::array<int32_t, 6> kAndroidAxes = {0, 1, 11, 14, 17, 18};
constexpr std::array<uint8_t, 14> kButtons = {
    SDL_GAMEPAD_BUTTON_SOUTH,          SDL_GAMEPAD_BUTTON_EAST,
    SDL_GAMEPAD_BUTTON_WEST,           SDL_GAMEPAD_BUTTON_NORTH,
    SDL_GAMEPAD_BUTTON_DPAD_UP,        SDL_GAMEPAD_BUTTON_DPAD_DOWN,
    SDL_GAMEPAD_BUTTON_DPAD_LEFT,      SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
    SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
    SDL_GAMEPAD_BUTTON_LEFT_STICK,     SDL_GAMEPAD_BUTTON_RIGHT_STICK,
    SDL_GAMEPAD_BUTTON_BACK,           SDL_GAMEPAD_BUTTON_START};

bool Has(uint32_t mask, uint8_t index) {
  return index < 32 && (mask & (uint32_t{1} << index)) != 0;
}

RobloxInputDispatchResult State(RobloxInputEventKind kind) {
  return {RobloxInputDispatchState::kStateUpdated, kind, Status::Ok()};
}

RobloxInputDispatchResult Ignored(RobloxInputEventKind kind,
                                  const char* message) {
  return {RobloxInputDispatchState::kIgnoredUnsupported, kind,
          Status::Error(StatusCode::kUnsupported, message)};
}

void KeepFirstError(Status status, Status* first_error) {
  if (first_error->ok()) {
    *first_error = std::move(status);
  }
}

float SignedAxis(int16_t value) {
  return static_cast<float>(value) / (value < 0 ? 32768.0F : 32767.0F);
}

std::array<float, 2> Stick(int16_t raw_x, int16_t raw_y) {
  constexpr float kDeadzone = 0.20F;
  const float x = SignedAxis(raw_x);
  const float y = -SignedAxis(raw_y);
  const float length = std::sqrt(x * x + y * y);
  if (length <= kDeadzone) {
    return {};
  }
  const float scale =
      (std::min(length, 1.0F) - kDeadzone) / ((1.0F - kDeadzone) * length);
  return {x * scale, y * scale};
}

float Trigger(int16_t raw) {
  constexpr float kDeadzone = 0.02F;
  const float value = std::max(0.0F, static_cast<float>(raw) / 32767.0F);
  return value <= kDeadzone ? 0.0F : (value - kDeadzone) / (1.0F - kDeadzone);
}

}  // namespace

int32_t MapSdlGamepadButtonToAndroid(uint8_t button) {
  switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH:
      return 96;
    case SDL_GAMEPAD_BUTTON_EAST:
      return 97;
    case SDL_GAMEPAD_BUTTON_WEST:
      return 99;
    case SDL_GAMEPAD_BUTTON_NORTH:
      return 100;
    case SDL_GAMEPAD_BUTTON_DPAD_UP:
      return 19;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
      return 20;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
      return 21;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
      return 22;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
      return 102;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
      return 103;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK:
      return 106;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
      return 107;
    case SDL_GAMEPAD_BUTTON_START:
      return 108;
    case SDL_GAMEPAD_BUTTON_BACK:
      return 109;
    default:
      return -1;
  }
}

int32_t RobloxGamepadType(platform::GamepadFamily family) {
  // NativeInputInterface's Android gamepad types are not SDL_GamepadType.
  switch (family) {
    case platform::GamepadFamily::kPlayStation4:
      return 1;
    case platform::GamepadFamily::kPlayStation5:
      return 2;
    case platform::GamepadFamily::kXbox:
      return 3;
    default:
      return 0;
  }
}

std::vector<RobloxInputRouter::ActiveGamepad>::iterator
RobloxInputRouter::FindGamepadLocked(int64_t instance_id) {
  return std::find_if(gamepads_.begin(), gamepads_.end(),
                      [instance_id](const ActiveGamepad& gamepad) {
                        return gamepad.instance_id == instance_id;
                      });
}

RobloxInputDispatchResult RobloxInputRouter::HandleGamepadConnectionLocked(
    const platform::GamepadConnectionEvent& event) {
  constexpr auto kind = RobloxInputEventKind::kGamepadConnection;
  if (!sink_.gamepad.valid()) {
    return Ignored(kind, "Roblox gamepad export table is incomplete");
  }
  if (event.instance_id <= 0) {
    return {RobloxInputDispatchState::kRejectedInvalid, kind,
            Status::Error(StatusCode::kInvalidArgument, "invalid gamepad ID")};
  }
  auto found = FindGamepadLocked(event.instance_id);
  Status status = Status::Ok();
  if (found != gamepads_.end()) {
    if (event.connected && !event.remapped) {
      return State(kind);
    }
    status = DisconnectGamepadLocked(*found);
    gamepads_.erase(found);
    snapshot_.detected_gamepads = gamepads_.size();
  }
  if (event.connected) {
    int32_t device_id = 1;
    while (std::any_of(gamepads_.begin(), gamepads_.end(),
                       [device_id](const ActiveGamepad& gamepad) {
                         return gamepad.device_id == device_id;
                       })) {
      if (device_id == std::numeric_limits<int32_t>::max()) {
        return {RobloxInputDispatchState::kRejectedInvalid, kind,
                Status::Error(StatusCode::kUnavailable,
                              "Roblox gamepad device IDs exhausted")};
      }
      ++device_id;
    }
    ActiveGamepad gamepad;
    gamepad.instance_id = event.instance_id;
    gamepad.device_id = device_id;
    gamepad.descriptor = event.descriptor;
    gamepads_.push_back(std::move(gamepad));
    snapshot_.detected_gamepads = gamepads_.size();
    std::fprintf(stderr,
                 "  [gamepad] detected SDL device %lld: %.160s (type=%d); "
                 "ready for input\n",
                 static_cast<long long>(event.instance_id),
                 event.descriptor.name.c_str(),
                 RobloxGamepadType(event.descriptor.family));
  }
  return status.ok() ? State(kind)
                     : NativeResultLocked(std::move(status), kind);
}

Status RobloxInputRouter::ConfigureGamepadLocked(const ActiveGamepad& gamepad,
                                                 bool available) {
  const auto& sink = sink_.gamepad;
  const int32_t type =
      available ? RobloxGamepadType(gamepad.descriptor.family) : 0;
  Status first_error = Status::Ok();
  for (const uint8_t button : kButtons) {
    KeepFirstError(
        sink.supported_key(sink.context, gamepad.device_id,
                           MapSdlGamepadButtonToAndroid(button),
                           available && Has(gamepad.descriptor.buttons, button),
                           type),
        &first_error);
  }
  for (uint8_t axis = 0; axis < kAndroidAxes.size(); ++axis) {
    KeepFirstError(sink.supported_motion(
                       sink.context, gamepad.device_id, kAndroidAxes[axis], -1,
                       available && Has(gamepad.descriptor.axes, axis), type),
                   &first_error);
  }
  // SDL has already normalized trigger aliases; D-pad uses key events only.
  for (const int32_t axis : {22, 23, 15, 16}) {
    KeepFirstError(sink.supported_motion(sink.context, gamepad.device_id, axis,
                                         -1, false, type),
                   &first_error);
    if (axis == 15 || axis == 16) {
      KeepFirstError(sink.supported_motion(sink.context, gamepad.device_id,
                                           axis, 1, false, type),
                     &first_error);
    }
  }
  return first_error;
}

Status RobloxInputRouter::ConnectGamepadLocked(ActiveGamepad& gamepad) {
  if (gamepad.connected) {
    return Status::Ok();
  }
  // Android announces capabilities, then connection, then the first action.
  Status status = ConfigureGamepadLocked(gamepad, true);
  if (status.ok()) {
    status =
        sink_.gamepad.connect(sink_.gamepad.context, gamepad.device_id,
                              RobloxGamepadType(gamepad.descriptor.family));
  }
  if (!status.ok()) {
    (void)ConfigureGamepadLocked(gamepad, false);
    (void)sink_.gamepad.disconnect(sink_.gamepad.context, gamepad.device_id);
    return status;
  }
  gamepad.connected = true;
  ++snapshot_.connected_gamepads;
  std::fprintf(stderr, "  [gamepad] connected to Roblox: device=%d type=%d\n",
               gamepad.device_id, RobloxGamepadType(gamepad.descriptor.family));
  return Status::Ok();
}

RobloxInputDispatchResult RobloxInputRouter::HandleGamepadButtonLocked(
    const platform::GamepadButtonEvent& event) {
  constexpr auto kind = RobloxInputEventKind::kGamepadButton;
  auto found = FindGamepadLocked(event.instance_id);
  if (!sink_.gamepad.valid() || found == gamepads_.end()) {
    return Ignored(kind, "gamepad is not available");
  }
  if (!snapshot_.focused) {
    return {RobloxInputDispatchState::kIgnoredUnfocused, kind, Status::Ok()};
  }
  const int32_t key = MapSdlGamepadButtonToAndroid(event.button);
  if (key < 0 || !Has(found->descriptor.buttons, event.button)) {
    return Ignored(kind, "gamepad button is not supported");
  }
  if (Has(found->pressed_buttons, event.button) == event.pressed) {
    return State(kind);
  }
  Status status = ConnectGamepadLocked(*found);
  if (status.ok()) {
    status = sink_.gamepad.button(sink_.gamepad.context, found->device_id, key,
                                  event.pressed);
  }
  if (status.ok()) {
    const uint32_t mask = uint32_t{1} << event.button;
    found->pressed_buttons = event.pressed ? found->pressed_buttons | mask
                                           : found->pressed_buttons & ~mask;
  }
  return NativeResultLocked(std::move(status), kind);
}

Status RobloxInputRouter::DispatchGamepadAxesLocked(ActiveGamepad& gamepad,
                                                    uint8_t axis,
                                                    bool* dispatched) {
  *dispatched = false;
  const uint8_t first = axis < 4 ? static_cast<uint8_t>(axis & ~1U) : axis;
  const uint8_t count = axis < 4 ? 2 : 1;
  const auto stick =
      axis < 4 ? Stick(gamepad.raw_axes[first], gamepad.raw_axes[first + 1])
               : std::array<float, 2>{Trigger(gamepad.raw_axes[axis]), 0};
  if (gamepad.sent_axes[first] == stick[0] &&
      (count == 1 || gamepad.sent_axes[first + 1] == stick[1])) {
    return Status::Ok();
  }
  Status status = ConnectGamepadLocked(gamepad);
  if (!status.ok()) {
    return status;
  }
  for (uint8_t index = first; index < first + count; ++index) {
    if (!Has(gamepad.descriptor.axes, index)) {
      continue;
    }
    status = sink_.gamepad.axis(sink_.gamepad.context, gamepad.device_id,
                                kAndroidAxes[index], count == 2 ? stick[0] : 0,
                                count == 2 ? stick[1] : 0,
                                count == 1 ? stick[0] : 0);
    if (!status.ok()) {
      return status;
    }
    *dispatched = true;
  }
  gamepad.sent_axes[first] = stick[0];
  if (count == 2) {
    gamepad.sent_axes[first + 1] = stick[1];
  }
  return Status::Ok();
}

RobloxInputDispatchResult RobloxInputRouter::HandleGamepadAxisLocked(
    const platform::GamepadAxisEvent& event) {
  constexpr auto kind = RobloxInputEventKind::kGamepadAxis;
  auto found = FindGamepadLocked(event.instance_id);
  if (!sink_.gamepad.valid() || found == gamepads_.end()) {
    return Ignored(kind, "gamepad is not available");
  }
  if (!snapshot_.focused) {
    return {RobloxInputDispatchState::kIgnoredUnfocused, kind, Status::Ok()};
  }
  if (event.axis >= kAndroidAxes.size() ||
      !Has(found->descriptor.axes, event.axis)) {
    return Ignored(kind, "gamepad axis is not supported");
  }
  found->raw_axes[event.axis] = event.value;
  bool dispatched = false;
  Status status = DispatchGamepadAxesLocked(*found, event.axis, &dispatched);
  return status.ok() && !dispatched
             ? State(kind)
             : NativeResultLocked(std::move(status), kind);
}

Status RobloxInputRouter::ReleaseGamepadInputsLocked(ActiveGamepad& gamepad) {
  Status first_error = Status::Ok();
  if (gamepad.connected) {
    for (const uint8_t button : kButtons) {
      if (Has(gamepad.pressed_buttons, button)) {
        Status status =
            sink_.gamepad.button(sink_.gamepad.context, gamepad.device_id,
                                 MapSdlGamepadButtonToAndroid(button), false);
        if (status.ok()) {
          ++snapshot_.gamepad_events;
        }
        KeepFirstError(std::move(status), &first_error);
      }
    }
    gamepad.raw_axes.fill(0);
    for (const uint8_t axis : {0, 2, 4, 5}) {
      bool dispatched = false;
      Status status = DispatchGamepadAxesLocked(gamepad, axis, &dispatched);
      if (status.ok() && dispatched) {
        ++snapshot_.gamepad_events;
      }
      KeepFirstError(std::move(status), &first_error);
    }
  }
  gamepad.pressed_buttons = 0;
  gamepad.raw_axes.fill(0);
  gamepad.sent_axes.fill(0);
  return first_error;
}

Status RobloxInputRouter::DisconnectGamepadLocked(ActiveGamepad& gamepad) {
  Status first_error = ReleaseGamepadInputsLocked(gamepad);
  if (gamepad.connected) {
    KeepFirstError(ConfigureGamepadLocked(gamepad, false), &first_error);
    KeepFirstError(
        sink_.gamepad.disconnect(sink_.gamepad.context, gamepad.device_id),
        &first_error);
    gamepad.connected = false;
    --snapshot_.connected_gamepads;
    std::fprintf(stderr, "  [gamepad] disconnected from Roblox: device=%d\n",
                 gamepad.device_id);
  }
  return first_error;
}

Status RobloxInputRouter::DisconnectGamepadsLocked() {
  Status first_error = Status::Ok();
  for (ActiveGamepad& gamepad : gamepads_) {
    KeepFirstError(DisconnectGamepadLocked(gamepad), &first_error);
  }
  gamepads_.clear();
  snapshot_.detected_gamepads = 0;
  return first_error;
}

}  // namespace runtime
}  // namespace mocktail
