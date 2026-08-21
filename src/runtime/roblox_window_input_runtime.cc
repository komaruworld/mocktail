#include "runtime/roblox_window_input_runtime.h"

#include <SDL3/SDL_video.h>

#include <cstdio>
#include <string>
#include <utility>

#include "window/window.h"

namespace mocktail {
namespace runtime {

RobloxWindowInputRuntime::RobloxWindowInputRuntime(
    JniEnvironmentProvider environment, RobloxInputSymbols symbols)
    : text_clipboard_(platform::CreateSdlTextClipboard()),
      runtime_(environment, symbols, text_surface_overlay_.sink(),
               text_clipboard_.get()) {}

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
      {window_viewport.logical_width, window_viewport.logical_height});
  if (!status.ok()) {
    return status;
  }
  const bool initially_focused =
      (SDL_GetWindowFlags(sdl_window) & SDL_WINDOW_INPUT_FOCUS) != 0;
  status = runtime_.Initialize(
      {window_viewport.logical_width, window_viewport.logical_height,
       window_viewport.pixel_width, window_viewport.pixel_height},
      initially_focused);
  if (!status.ok()) {
    (void)text_surface_overlay_.Shutdown();
    return status;
  }
  if (!window::SetPlatformEventObserver(
          &RobloxInputRuntime::PlatformEventCallback, &runtime_)) {
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
  return Status::Ok();
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
