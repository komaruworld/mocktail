#include "runtime/roblox_window_input_runtime.h"

#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>

#include <cstdio>
#include <string>
#include <utility>
#include <variant>

#include "window/window.h"

namespace mocktail {
namespace runtime {

RobloxWindowInputRuntime::RobloxWindowInputRuntime(
    JniEnvironmentProvider environment, RobloxInputSymbols symbols,
    std::shared_ptr<RobloxHaptics> haptics)
    : text_clipboard_(platform::CreateSdlTextClipboard()),
      runtime_(environment, symbols, text_surface_overlay_.sink(),
               text_clipboard_.get()),
      haptics_(std::move(haptics)) {}

RobloxWindowInputRuntime::~RobloxWindowInputRuntime() { Shutdown(); }

Status RobloxWindowInputRuntime::Initialize() {
  if (observer_registered_) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "window input runtime is already initialized");
  }
  const window::WindowViewportSnapshot window_viewport =
      window::GetWindowViewportSnapshot();
  if (!window_viewport.valid()) {
    return Status::Error(StatusCode::kUnavailable,
                         "SDL input viewport is unavailable");
  }
  auto* sdl_window = static_cast<SDL_Window*>(window::GetBackendWindow());
  if (sdl_window == nullptr) {
    return Status::Error(StatusCode::kUnavailable,
                         "SDL input window is unavailable");
  }
  Status status = text_surface_overlay_.Initialize(
      {window_viewport.pixel_width, window_viewport.pixel_height,
       window_viewport.dpi_scale});
  if (!status.ok()) {
    return status;
  }
  const bool initially_focused =
      (SDL_GetWindowFlags(sdl_window) & SDL_WINDOW_INPUT_FOCUS) != 0;
  status = runtime_.Initialize(
      {window_viewport.logical_width, window_viewport.logical_height,
       window_viewport.pixel_width, window_viewport.pixel_height,
       window_viewport.dpi_scale},
      initially_focused);
  if (!status.ok()) {
    (void)text_surface_overlay_.Shutdown();
    return status;
  }
  if (!window::SetPlatformEventObserver(
          &RobloxWindowInputRuntime::PlatformEventCallback, this)) {
    runtime_.Deactivate();
    (void)text_surface_overlay_.Shutdown();
    return Status::Error(StatusCode::kFailedPrecondition,
                         "SDL platform event observer is unavailable");
  }
  observer_registered_ = true;
  if (!window::SetMouseLockQueryCallback(
          &RobloxWindowInputRuntime::MouseLockQueryCallback, this)) {
    window::ClearPlatformEventObserver();
    observer_registered_ = false;
    runtime_.Deactivate();
    (void)text_surface_overlay_.Shutdown();
    return Status::Error(StatusCode::kFailedPrecondition,
                         "SDL mouse lock query owner is unavailable");
  }
  mouse_lock_query_registered_ = true;
  initialized_ = true;
  if (runtime_.SupportsGamepads() || haptics_) {
    const Status gamepad_status = gamepads_.Initialize(
        this, &RobloxWindowInputRuntime::GamepadEventCallback);
    if (!gamepad_status.ok()) {
      std::fprintf(stderr, "  [gamepad] unavailable: %s\n",
                   gamepad_status.message().c_str());
    }
  }
  if (haptics_) {
    haptics_->SetFocused(initially_focused);
    haptics_target_ = gamepads_.RumbleTarget();
    haptics_->SetAvailable(haptics_target_ != 0);
    haptics_pump_registered_ = window::SetHapticsPumpCallback(
        &RobloxWindowInputRuntime::HapticsPump, this);
    if (!haptics_pump_registered_) {
      (void)Shutdown();
      return Status::Error(StatusCode::kUnavailable,
                           "haptics pump is unavailable");
    }
  }
  return Status::Ok();
}

void RobloxWindowInputRuntime::SynchronizeHapticsTarget() {
  if (!haptics_) {
    return;
  }
  const auto target = gamepads_.RumbleTarget();
  if (target != haptics_target_) {
    haptics_->Cancel();
    gamepads_.StopRumble();
    last_rumble_ = 0;
    haptics_target_ = target;
  }
  haptics_->SetAvailable(target != 0);
}

bool RobloxWindowInputRuntime::HapticsPump(void* context) {
  auto* self = static_cast<RobloxWindowInputRuntime*>(context);
  self->SynchronizeHapticsTarget();
  const uint64_t now = SDL_GetTicksNS();
  const float value = self->haptics_->Sample(now);
  if (value != self->last_rumble_ ||
      (value > 0 && now - self->last_rumble_ns_ >= 100000000)) {
    self->gamepads_.Rumble(value, value > 0 ? 250 : 0);
    self->last_rumble_ = value;
    self->last_rumble_ns_ = now;
  }
  return true;
}

void RobloxWindowInputRuntime::GamepadEventCallback(
    void* context, const platform::PlatformEvent& event) {
  auto* self = static_cast<RobloxWindowInputRuntime*>(context);
  (void)self->runtime_.HandleEvent(event);
}

void RobloxWindowInputRuntime::PlatformEventCallback(
    void* context, const platform::PlatformEvent& event) {
  if (context == nullptr) {
    return;
  }
  auto* self = static_cast<RobloxWindowInputRuntime*>(context);
  if (self->gamepads_.HandleEvent(event)) {
    self->SynchronizeHapticsTarget();
    return;
  }
  if (const auto* resized =
          std::get_if<platform::WindowResizedEvent>(&event.payload)) {
    (void)self->text_surface_overlay_.UpdateViewport(
        {resized->pixel_width, resized->pixel_height, resized->density_scale});
  }
  (void)self->runtime_.HandleEvent(event);
  if (const auto* focus =
          std::get_if<platform::WindowFocusEvent>(&event.payload)) {
    if (self->haptics_) {
      self->haptics_->SetFocused(focus->focused);
    }
    if (!focus->focused) {
      self->gamepads_.StopRumble();
      self->last_rumble_ = 0;
    }
    if (focus->focused) {
      self->gamepads_.ResendState();
    }
  }
}

Status RobloxWindowInputRuntime::Shutdown() {
  if (!initialized_) {
    return Status::Ok();
  }
  if (observer_registered_) {
    window::ClearPlatformEventObserver();
    observer_registered_ = false;
  }
  if (mouse_lock_query_registered_) {
    window::ClearMouseLockQueryCallback();
    mouse_lock_query_registered_ = false;
  }
  if (haptics_pump_registered_) {
    window::ClearHapticsPumpCallback();
    haptics_pump_registered_ = false;
  }
  if (haptics_) {
    haptics_->Cancel();
    haptics_->SetAvailable(false);
  }
  gamepads_.StopRumble();
  gamepads_.Shutdown();
  const RobloxInputSnapshot snapshot = runtime_.Snapshot();
  Status status = runtime_.Deactivate();
  Status overlay_status = text_surface_overlay_.Shutdown();
  initialized_ = false;
  if (!status.ok()) {
    return status;
  }
  if (!overlay_status.ok()) {
    return overlay_status;
  }
  if (snapshot.native_errors != 0) {
    return Status::Error(StatusCode::kPlatformError,
                         "Roblox native input reported " +
                             std::to_string(snapshot.native_errors) +
                             " dispatch error(s)");
  }
  std::fprintf(stderr,
               "  [input] typed production input stopped after observer "
               "clear\n");
  return Status::Ok();
}

Status RobloxWindowInputRuntime::BeginTextFocusSession(
    RobloxTextFocusSession session) {
  if (!initialized_) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "window input runtime is not initialized");
  }
  return runtime_.BeginTextFocusSession(std::move(session));
}

Status RobloxWindowInputRuntime::EndTextFocusSession(int64_t textbox_handle,
                                                     uint64_t generation,
                                                     bool notify_native) {
  if (!initialized_) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "window input runtime is not initialized");
  }
  return runtime_.EndTextFocusSession(textbox_handle, generation,
                                      notify_native);
}

Status RobloxWindowInputRuntime::ReplaceFocusedTextFromEngine(
    uint64_t generation, std::string authoritative_utf8) {
  if (!initialized_) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "window input runtime is not initialized");
  }
  return runtime_.ReplaceFocusedTextFromEngine(generation,
                                               std::move(authoritative_utf8));
}

Status RobloxWindowInputRuntime::QueryCurrentTextBoxInfo(
    RobloxNativeTextBoxInfoQueryResult* result) {
  if (!initialized_) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "window input runtime is not initialized");
  }
  return runtime_.QueryCurrentTextBoxInfo(result);
}

Status RobloxWindowInputRuntime::UpdateTextFocusProperties(
    uint64_t generation, const RobloxTextFocusProperties& properties) {
  if (!initialized_) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "window input runtime is not initialized");
  }
  return runtime_.UpdateTextFocusProperties(generation, properties);
}

RobloxInputSnapshot RobloxWindowInputRuntime::Snapshot() const {
  return runtime_.Snapshot();
}

bool RobloxWindowInputRuntime::MouseLockQueryCallback(void* context,
                                                      bool* locked_center) {
  if (context == nullptr || locked_center == nullptr) {
    return false;
  }
  return static_cast<RobloxWindowInputRuntime*>(context)
      ->runtime_.GetMainWindowIsMouseLockedCenter(locked_center)
      .ok();
}

}  // namespace runtime
}  // namespace mocktail
