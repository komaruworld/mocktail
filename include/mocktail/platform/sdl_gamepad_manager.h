#ifndef MOCKTAIL_PLATFORM_SDL_GAMEPAD_MANAGER_H_
#define MOCKTAIL_PLATFORM_SDL_GAMEPAD_MANAGER_H_

#include <SDL3/SDL_gamepad.h>

#include <map>

#include "mocktail/platform/platform_runtime.h"

namespace mocktail {
namespace platform {

// Main-thread owner. Consumes events from the existing SDL pump; it never
// creates another event loop or consumes keyboard/window events.
class SdlGamepadManager final {
 public:
  using EventFn = void (*)(void*, const PlatformEvent&);

  ~SdlGamepadManager();
  SdlGamepadManager() = default;
  SdlGamepadManager(const SdlGamepadManager&) = delete;
  SdlGamepadManager& operator=(const SdlGamepadManager&) = delete;

  Status Initialize(void* context, EventFn emit);
  void Shutdown();
  bool HandleEvent(const PlatformEvent& event);
  void ResendState();

 private:
  SDL_Gamepad* Open(SDL_JoystickID id);
  void Remove(SDL_JoystickID id);
  void Describe(SDL_JoystickID id, SDL_Gamepad* gamepad, bool remapped);
  void EmitState(SDL_JoystickID id, SDL_Gamepad* gamepad);
  void Emit(PlatformEventPayload payload);

  std::map<SDL_JoystickID, SDL_Gamepad*> gamepads_;
  void* context_ = nullptr;
  EventFn emit_ = nullptr;
  bool initialized_ = false;
};

}  // namespace platform
}  // namespace mocktail

#endif  // MOCKTAIL_PLATFORM_SDL_GAMEPAD_MANAGER_H_
