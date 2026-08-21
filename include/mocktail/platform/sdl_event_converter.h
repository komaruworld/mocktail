#ifndef MOCKTAIL_PLATFORM_SDL_EVENT_CONVERTER_H_
#define MOCKTAIL_PLATFORM_SDL_EVENT_CONVERTER_H_

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_video.h>

#include "mocktail/platform/platform_runtime.h"

namespace mocktail {
namespace platform {

// Converts one SDL event without retaining either argument. The caller remains
// the sole owner of SDL_PollEvent and decides how converted events are routed.
// Returns false for invalid arguments and SDL events outside the platform
// contract.
bool ConvertSdlEvent(SDL_Window* window, const SDL_Event& source,
                     PlatformEvent* destination);

}  // namespace platform
}  // namespace mocktail

#endif  // MOCKTAIL_PLATFORM_SDL_EVENT_CONVERTER_H_
