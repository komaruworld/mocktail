#include "mocktail/platform/sdl_event_converter.h"

#include <SDL3/SDL_mouse.h>

#include <cstdint>

namespace mocktail {
namespace platform {

bool ConvertSdlEvent(SDL_Window* window, const SDL_Event& source,
                     PlatformEvent* destination) {
  if (window == nullptr || destination == nullptr) {
    return false;
  }

  destination->timestamp_ns = source.common.timestamp;
  switch (source.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
      destination->payload = QuitEvent{};
      return true;
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: {
      WindowResizedEvent resized;
      if (!SDL_GetWindowSize(window, &resized.logical_width,
                             &resized.logical_height) ||
          !SDL_GetWindowSizeInPixels(window, &resized.pixel_width,
                                     &resized.pixel_height)) {
        return false;
      }
      destination->payload = resized;
      return true;
    }
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
      destination->payload = WindowFocusEvent{true};
      return true;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
      destination->payload = WindowFocusEvent{false};
      return true;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
      destination->payload =
          KeyEvent{source.key.down, source.key.repeat,
                   static_cast<std::uint32_t>(source.key.scancode),
                   static_cast<std::uint32_t>(source.key.key),
                   static_cast<std::uint32_t>(source.key.mod)};
      return true;
    case SDL_EVENT_TEXT_INPUT:
      destination->payload =
          TextInputEvent{source.text.text != nullptr ? source.text.text : ""};
      return true;
    case SDL_EVENT_TEXT_EDITING:
      destination->payload =
          TextEditingEvent{source.edit.text != nullptr ? source.edit.text : "",
                           source.edit.start, source.edit.length};
      return true;
    case SDL_EVENT_MOUSE_MOTION:
      destination->payload = MouseMotionEvent{
          source.motion.x, source.motion.y, source.motion.xrel,
          source.motion.yrel, static_cast<std::uint32_t>(source.motion.state)};
      return true;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
      destination->payload = MouseButtonEvent{
          source.button.down, source.button.button, source.button.clicks,
          source.button.x, source.button.y};
      return true;
    case SDL_EVENT_MOUSE_WHEEL: {
      float delta_x = source.wheel.x;
      float delta_y = source.wheel.y;
      if (source.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
        delta_x = -delta_x;
        delta_y = -delta_y;
      }
      destination->payload = MouseWheelEvent{
          delta_x, delta_y, source.wheel.mouse_x, source.wheel.mouse_y};
      return true;
    }
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED: {
      TouchEvent::Action action = TouchEvent::Action::kMove;
      if (source.type == SDL_EVENT_FINGER_DOWN) {
        action = TouchEvent::Action::kDown;
      } else if (source.type == SDL_EVENT_FINGER_UP) {
        action = TouchEvent::Action::kUp;
      } else if (source.type == SDL_EVENT_FINGER_CANCELED) {
        action = TouchEvent::Action::kCancelled;
      }
      destination->payload =
          TouchEvent{action,
                     static_cast<std::int64_t>(source.tfinger.touchID),
                     static_cast<std::int64_t>(source.tfinger.fingerID),
                     source.tfinger.x,
                     source.tfinger.y,
                     source.tfinger.dx,
                     source.tfinger.dy,
                     source.tfinger.pressure};
      return true;
    }
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
      destination->payload = GamepadConnectionEvent{
          static_cast<std::int64_t>(source.gdevice.which),
          source.type == SDL_EVENT_GAMEPAD_ADDED};
      return true;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
      destination->payload =
          GamepadAxisEvent{static_cast<std::int64_t>(source.gaxis.which),
                           source.gaxis.axis, source.gaxis.value};
      return true;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
      destination->payload =
          GamepadButtonEvent{static_cast<std::int64_t>(source.gbutton.which),
                             source.gbutton.button, source.gbutton.down};
      return true;
    default:
      return false;
  }
}

}  // namespace platform
}  // namespace mocktail
