#include "mocktail/platform/sdl_platform_runtime.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_video.h>

#include <cstdio>
#include <memory>
#include <string>
#include <utility>

#include "mocktail/platform/sdl_application_metadata.h"
#include "mocktail/platform/sdl_event_converter.h"
#include "mocktail/platform/sdl_window_icon.h"

namespace mocktail {
namespace platform {
namespace {

Status SdlError(const char* operation) {
  std::string message = operation != nullptr ? operation : "SDL operation";
  message += " failed: ";
  const char* error = SDL_GetError();
  message += error != nullptr && error[0] != '\0' ? error : "unknown SDL error";
  return Status::Error(StatusCode::kPlatformError, std::move(message));
}

class SdlPlatformRuntime final : public PlatformRuntime {
 public:
  ~SdlPlatformRuntime() override { Shutdown(); }

  Status Initialize(const WindowOptions& options) override {
    if (initialized_) {
      return Status::Error(StatusCode::kFailedPrecondition,
                           "SDL platform runtime is already initialized");
    }
    if (options.width <= 0 || options.height <= 0) {
      return Status::Error(StatusCode::kInvalidArgument,
                           "window dimensions must be positive");
    }
    if (options.title.empty()) {
      return Status::Error(StatusCode::kInvalidArgument,
                           "window title must not be empty");
    }

    Status metadata_status = ConfigureSdlApplicationMetadata();
    if (!metadata_status.ok()) {
      return metadata_status;
    }

    const SDL_InitFlags init_flags = SDL_INIT_VIDEO | SDL_INIT_GAMEPAD;
    if (!SDL_InitSubSystem(init_flags)) {
      return SdlError("SDL_InitSubSystem");
    }
    initialized_subsystems_ = init_flags;

    SDL_WindowFlags flags = options.high_pixel_density
                                ? SDL_WINDOW_HIGH_PIXEL_DENSITY
                                : static_cast<SDL_WindowFlags>(0);
    if (options.initially_hidden) {
      flags |= SDL_WINDOW_HIDDEN;
    }
    if (options.surface_api == WindowSurfaceApi::kDirectVulkan) {
      flags |= SDL_WINDOW_VULKAN;
    } else {
      // SDL owns only the native window. ANGLE/Roblox owns the EGL context.
      flags |= SDL_WINDOW_OPENGL;
    }

    window_ = SDL_CreateWindow(options.title.c_str(), options.width,
                               options.height, flags);
    if (window_ == nullptr) {
      Status status = SdlError("SDL_CreateWindow");
      Shutdown();
      return status;
    }
    const Status icon_status = ApplySdlWindowIcon(window_);
    if (!icon_status.ok()) {
      std::fprintf(stderr, "  [window] SDL window icon unavailable: %s\n",
                   icon_status.message().c_str());
    }

    descriptor_ = ResolveNativeWindow(options.surface_api);
    if (options.require_native_window && !descriptor_.HasPlatformHandle()) {
      Shutdown();
      return Status::Error(
          StatusCode::kUnavailable,
          "SDL window has no X11 or Wayland native handle; refusing fake WSI");
    }

    initialized_ = true;
    return Status::Ok();
  }

  void Shutdown() override {
    if (window_ != nullptr) {
      SDL_DestroyWindow(window_);
      window_ = nullptr;
    }
    descriptor_ = {};
    if (initialized_subsystems_ != 0) {
      SDL_QuitSubSystem(initialized_subsystems_);
      initialized_subsystems_ = 0;
    }
    initialized_ = false;
  }

  bool IsInitialized() const override { return initialized_; }

  Status PollEvent(PlatformEvent* event, bool* has_event) override {
    if (event == nullptr || has_event == nullptr) {
      return Status::Error(StatusCode::kInvalidArgument,
                           "PollEvent output pointers must not be null");
    }
    if (!initialized_) {
      return Status::Error(StatusCode::kFailedPrecondition,
                           "platform runtime is not initialized");
    }

    *has_event = false;
    SDL_Event sdl_event;
    while (SDL_PollEvent(&sdl_event)) {
      if (ConvertSdlEvent(window_, sdl_event, event)) {
        *has_event = true;
        return Status::Ok();
      }
    }
    return Status::Ok();
  }

  Status SetTextInputEnabled(bool enabled) override {
    if (!initialized_ || window_ == nullptr) {
      return Status::Error(StatusCode::kFailedPrecondition,
                           "platform runtime is not initialized");
    }
    const bool success =
        enabled ? SDL_StartTextInput(window_) : SDL_StopTextInput(window_);
    return success
               ? Status::Ok()
               : SdlError(enabled ? "SDL_StartTextInput" : "SDL_StopTextInput");
  }

  Status ShowWindow() override {
    if (!initialized_ || window_ == nullptr) {
      return Status::Error(StatusCode::kFailedPrecondition,
                           "platform runtime is not initialized");
    }
    if (!SDL_ShowWindow(window_)) {
      return SdlError("SDL_ShowWindow");
    }
    return Status::Ok();
  }

  NativeWindowDescriptor GetNativeWindow() const override {
    return descriptor_;
  }

 private:
  NativeWindowDescriptor ResolveNativeWindow(WindowSurfaceApi surface_api) {
    NativeWindowDescriptor descriptor;
    descriptor.surface_api = surface_api;
    descriptor.backend_window = window_;

    const SDL_PropertiesID properties = SDL_GetWindowProperties(window_);
    if (properties == 0) {
      return descriptor;
    }

    void* x11_display = SDL_GetPointerProperty(
        properties, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
    const Sint64 x11_window =
        SDL_GetNumberProperty(properties, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    if (x11_display != nullptr && x11_window != 0) {
      descriptor.window_system = NativeWindowSystem::kX11;
      descriptor.native_display = x11_display;
      descriptor.native_window = static_cast<std::uintptr_t>(x11_window);
      return descriptor;
    }

    void* wayland_display = SDL_GetPointerProperty(
        properties, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
    void* wayland_surface = SDL_GetPointerProperty(
        properties, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
    if (wayland_display != nullptr && wayland_surface != nullptr) {
      descriptor.window_system = NativeWindowSystem::kWayland;
      descriptor.native_display = wayland_display;
      descriptor.native_surface = wayland_surface;
      descriptor.native_egl_window = SDL_GetPointerProperty(
          properties, SDL_PROP_WINDOW_WAYLAND_EGL_WINDOW_POINTER, nullptr);
    }
    return descriptor;
  }

  SDL_Window* window_ = nullptr;
  SDL_InitFlags initialized_subsystems_ = 0;
  NativeWindowDescriptor descriptor_;
  bool initialized_ = false;
};

}  // namespace

std::unique_ptr<PlatformRuntime> CreateSdlPlatformRuntime() {
  return std::make_unique<SdlPlatformRuntime>();
}

}  // namespace platform
}  // namespace mocktail
