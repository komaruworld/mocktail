#include "mocktail/platform/sdl_gamepad_manager.h"

#include <SDL3/SDL_init.h>
#include <SDL3/SDL_timer.h>

#include <cstdio>
#include <limits>
#include <utility>

namespace mocktail {
namespace platform {
namespace {

static_assert(SDL_GAMEPAD_BUTTON_COUNT <= 32);
static_assert(SDL_GAMEPAD_AXIS_COUNT <= 32);

GamepadFamily Family(SDL_Gamepad* gamepad) {
  switch (SDL_GetGamepadType(gamepad)) {
    case SDL_GAMEPAD_TYPE_XBOX360:
    case SDL_GAMEPAD_TYPE_XBOXONE:
      return GamepadFamily::kXbox;
    case SDL_GAMEPAD_TYPE_PS3:
    case SDL_GAMEPAD_TYPE_PS4:
      return GamepadFamily::kPlayStation4;
    case SDL_GAMEPAD_TYPE_PS5:
      return GamepadFamily::kPlayStation5;
    default:
      return GamepadFamily::kUnknown;
  }
}

bool ValidId(std::int64_t id) {
  return id > 0 && id <= std::numeric_limits<SDL_JoystickID>::max();
}

}  // namespace

SdlGamepadManager::~SdlGamepadManager() { Shutdown(); }

Status SdlGamepadManager::Initialize(void* context, EventFn emit) {
  if (initialized_ || emit == nullptr) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "gamepad owner is already initialized or has no sink");
  }
  if (!SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
    return Status::Error(
        StatusCode::kPlatformError,
        std::string("SDL gamepad initialization failed: ") + SDL_GetError());
  }
  initialized_ = true;
  context_ = context;
  emit_ = emit;
  SDL_SetGamepadEventsEnabled(true);
  int count = 0;
  SDL_JoystickID* ids = SDL_GetGamepads(&count);
  if (ids == nullptr) {
    const std::string error = SDL_GetError();
    Shutdown();
    return Status::Error(StatusCode::kPlatformError,
                         "SDL gamepad enumeration failed: " + error);
  }
  for (int index = 0; index < count; ++index) {
    Open(ids[index]);
  }
  SDL_free(ids);
  return Status::Ok();
}

void SdlGamepadManager::Shutdown() {
  if (!initialized_) {
    return;
  }
  while (!gamepads_.empty()) {
    Remove(gamepads_.begin()->first);
  }
  emit_ = nullptr;
  context_ = nullptr;
  initialized_ = false;
  SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}

void SdlGamepadManager::Emit(PlatformEventPayload payload) {
  if (emit_ != nullptr) {
    emit_(context_, {SDL_GetTicksNS(), std::move(payload)});
  }
}

void SdlGamepadManager::Describe(SDL_JoystickID id, SDL_Gamepad* gamepad,
                                 bool remapped) {
  GamepadDescriptor descriptor;
  descriptor.family = Family(gamepad);
  const char* name = SDL_GetGamepadName(gamepad);
  descriptor.name = name != nullptr ? name : "Unknown gamepad";
  for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT; ++button) {
    if (SDL_GamepadHasButton(gamepad, static_cast<SDL_GamepadButton>(button))) {
      descriptor.buttons |= std::uint32_t{1} << button;
    }
  }
  for (int axis = 0; axis < SDL_GAMEPAD_AXIS_COUNT; ++axis) {
    if (SDL_GamepadHasAxis(gamepad, static_cast<SDL_GamepadAxis>(axis))) {
      descriptor.axes |= std::uint32_t{1} << axis;
    }
  }
  Emit(GamepadConnectionEvent{id, true, std::move(descriptor), remapped});
}

SDL_Gamepad* SdlGamepadManager::Open(SDL_JoystickID id) {
  const auto found = gamepads_.find(id);
  if (found != gamepads_.end()) {
    return found->second;
  }
  SDL_Gamepad* gamepad = SDL_OpenGamepad(id);
  if (gamepad == nullptr) {
    std::fprintf(stderr, "  [gamepad] cannot open SDL device %u: %s\n", id,
                 SDL_GetError());
    return nullptr;
  }
  gamepads_.emplace(id, gamepad);
  Describe(id, gamepad, false);
  EmitState(id, gamepad);
  return gamepad;
}

void SdlGamepadManager::Remove(SDL_JoystickID id) {
  const auto found = gamepads_.find(id);
  if (found == gamepads_.end()) {
    return;
  }
  Emit(GamepadConnectionEvent{id, false, {}, false});
  SDL_CloseGamepad(found->second);
  gamepads_.erase(found);
}

void SdlGamepadManager::EmitState(SDL_JoystickID id, SDL_Gamepad* gamepad) {
  if (!SDL_GamepadConnected(gamepad)) {
    return;
  }
  for (int axis = 0; axis < SDL_GAMEPAD_AXIS_COUNT; ++axis) {
    const auto sdl_axis = static_cast<SDL_GamepadAxis>(axis);
    if (SDL_GamepadHasAxis(gamepad, sdl_axis)) {
      Emit(GamepadAxisEvent{id, static_cast<std::uint8_t>(axis),
                            SDL_GetGamepadAxis(gamepad, sdl_axis)});
    }
  }
  for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT; ++button) {
    const auto sdl_button = static_cast<SDL_GamepadButton>(button);
    if (SDL_GamepadHasButton(gamepad, sdl_button)) {
      Emit(GamepadButtonEvent{id, static_cast<std::uint8_t>(button),
                              SDL_GetGamepadButton(gamepad, sdl_button)});
    }
  }
}

void SdlGamepadManager::ResendState() {
  for (const auto& entry : gamepads_) {
    EmitState(entry.first, entry.second);
  }
}

bool SdlGamepadManager::HandleEvent(const PlatformEvent& event) {
  if (!initialized_) {
    return false;
  }
  if (const auto* connection =
          std::get_if<GamepadConnectionEvent>(&event.payload)) {
    if (!ValidId(connection->instance_id)) {
      return true;
    }
    const auto id = static_cast<SDL_JoystickID>(connection->instance_id);
    if (!connection->connected) {
      Remove(id);
    } else if (SDL_Gamepad* gamepad = Open(id)) {
      if (connection->remapped) {
        Describe(id, gamepad, true);
        EmitState(id, gamepad);
      }
    }
    return true;
  }
  std::int64_t id = 0;
  if (const auto* axis = std::get_if<GamepadAxisEvent>(&event.payload)) {
    id = axis->instance_id;
  } else if (const auto* button =
                 std::get_if<GamepadButtonEvent>(&event.payload)) {
    id = button->instance_id;
  } else {
    return false;
  }
  if (ValidId(id) && Open(static_cast<SDL_JoystickID>(id)) != nullptr) {
    emit_(context_, event);
  }
  return true;
}

}  // namespace platform
}  // namespace mocktail
