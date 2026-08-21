#include "mocktail/platform/sdl_window_icon.h"

#include <SDL3/SDL.h>

#include <string>
#include <utility>

#include "mocktail/platform/sdl_window_icon_data.h"

namespace mocktail {
namespace platform {
namespace {

Status IconError(const char* operation) {
  std::string message = operation;
  message += " failed: ";
  const char* error = SDL_GetError();
  message += error != nullptr && error[0] != '\0' ? error : "unknown SDL error";
  return Status::Error(StatusCode::kPlatformError, std::move(message));
}

}  // namespace

Status ApplySdlWindowIcon(SDL_Window* window) {
  if (window == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "SDL window must not be null");
  }

  SDL_IOStream* stream = SDL_IOFromConstMem(
      internal::kMocktailWindowIconPng,
      internal::kMocktailWindowIconPngSize);
  if (stream == nullptr) {
    return IconError("SDL_IOFromConstMem(window icon)");
  }

  SDL_Surface* icon = SDL_LoadPNG_IO(stream, true);
  if (icon == nullptr) {
    return IconError("SDL_LoadPNG_IO(window icon)");
  }

  if (!SDL_SetWindowIcon(window, icon)) {
    Status status = IconError("SDL_SetWindowIcon");
    SDL_DestroySurface(icon);
    return status;
  }
  SDL_DestroySurface(icon);
  return Status::Ok();
}

}  // namespace platform
}  // namespace mocktail
