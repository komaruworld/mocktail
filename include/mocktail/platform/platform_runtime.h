#ifndef MOCKTAIL_PLATFORM_PLATFORM_RUNTIME_H_
#define MOCKTAIL_PLATFORM_PLATFORM_RUNTIME_H_

#include <cstdint>
#include <string>
#include <variant>

#include "mocktail/status.h"

namespace mocktail {
namespace platform {

enum class WindowSurfaceApi {
  kAngleEgl = 0,
  kDirectVulkan,
};

enum class NativeWindowSystem {
  kUnknown = 0,
  kX11,
  kWayland,
};

struct WindowOptions {
  int width = 1280;
  int height = 720;
  std::string title = "Roblox";
  WindowSurfaceApi surface_api = WindowSurfaceApi::kAngleEgl;
  bool high_pixel_density = true;
  bool initially_hidden = true;

  // Production callers should keep this true. Tests using SDL's dummy driver
  // may opt out because that driver intentionally has no native WSI handle.
  bool require_native_window = true;
};

// Non-owning handles captured from SDL window properties. backend_window is
// an opaque SDL_Window pointer used only by SDL-specific graphics adapters.
struct NativeWindowDescriptor {
  NativeWindowSystem window_system = NativeWindowSystem::kUnknown;
  WindowSurfaceApi surface_api = WindowSurfaceApi::kAngleEgl;
  void* backend_window = nullptr;
  void* native_display = nullptr;
  std::uintptr_t native_window = 0;
  void* native_surface = nullptr;
  void* native_egl_window = nullptr;

  bool HasPlatformHandle() const {
    if (window_system == NativeWindowSystem::kX11) {
      return native_display != nullptr && native_window != 0;
    }
    if (window_system == NativeWindowSystem::kWayland) {
      return native_display != nullptr && native_surface != nullptr;
    }
    return false;
  }
};

struct QuitEvent {};

struct WindowResizedEvent {
  int logical_width = 0;
  int logical_height = 0;
  int pixel_width = 0;
  int pixel_height = 0;
};

struct WindowFocusEvent {
  bool focused = false;
};

struct KeyEvent {
  bool pressed = false;
  bool repeat = false;
  std::uint32_t scancode = 0;
  std::uint32_t keycode = 0;
  std::uint32_t modifiers = 0;
};

struct TextInputEvent {
  std::string text;
};

struct TextEditingEvent {
  std::string text;
  int start = 0;
  int length = 0;
};

struct MouseMotionEvent {
  float x = 0.0f;
  float y = 0.0f;
  float delta_x = 0.0f;
  float delta_y = 0.0f;
  std::uint32_t buttons = 0;
};

struct MouseButtonEvent {
  bool pressed = false;
  std::uint8_t button = 0;
  std::uint8_t clicks = 0;
  float x = 0.0f;
  float y = 0.0f;
};

struct MouseWheelEvent {
  float delta_x = 0.0f;
  float delta_y = 0.0f;
  float mouse_x = 0.0f;
  float mouse_y = 0.0f;
};

struct TouchEvent {
  enum class Action {
    kDown = 0,
    kMove,
    kUp,
    kCancelled,
  };

  Action action = Action::kMove;
  std::int64_t touch_id = 0;
  std::int64_t finger_id = 0;
  float x = 0.0f;
  float y = 0.0f;
  float delta_x = 0.0f;
  float delta_y = 0.0f;
  float pressure = 0.0f;
};

struct GamepadConnectionEvent {
  std::int64_t instance_id = 0;
  bool connected = false;
};

struct GamepadAxisEvent {
  std::int64_t instance_id = 0;
  std::uint8_t axis = 0;
  std::int16_t value = 0;
};

struct GamepadButtonEvent {
  std::int64_t instance_id = 0;
  std::uint8_t button = 0;
  bool pressed = false;
};

using PlatformEventPayload =
    std::variant<QuitEvent, WindowResizedEvent, WindowFocusEvent, KeyEvent,
                 TextInputEvent, TextEditingEvent, MouseMotionEvent,
                 MouseButtonEvent, MouseWheelEvent, TouchEvent,
                 GamepadConnectionEvent, GamepadAxisEvent, GamepadButtonEvent>;

struct PlatformEvent {
  std::uint64_t timestamp_ns = 0;
  PlatformEventPayload payload = QuitEvent{};
};

// Main-thread-owned platform boundary. Implementations never create an EGL or
// Vulkan device/context; those belong to the selected graphics backend.
class PlatformRuntime {
 public:
  virtual ~PlatformRuntime() = default;

  virtual Status Initialize(const WindowOptions& options) = 0;
  virtual void Shutdown() = 0;
  virtual bool IsInitialized() const = 0;

  // Returns OK with has_event=false when the queue currently contains no
  // supported platform event.
  virtual Status PollEvent(PlatformEvent* event, bool* has_event) = 0;
  virtual Status SetTextInputEnabled(bool enabled) = 0;
  virtual Status ShowWindow() = 0;

  virtual NativeWindowDescriptor GetNativeWindow() const = 0;
};

}  // namespace platform
}  // namespace mocktail

#endif  // MOCKTAIL_PLATFORM_PLATFORM_RUNTIME_H_
