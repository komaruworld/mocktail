#ifndef MOCKTAIL_PLATFORM_SDL_PLATFORM_RUNTIME_H_
#define MOCKTAIL_PLATFORM_SDL_PLATFORM_RUNTIME_H_

#include <memory>

#include "mocktail/platform/platform_runtime.h"

namespace mocktail {
namespace platform {

// Creates an SDL3-backed runtime without exposing SDL types to consumers.
std::unique_ptr<PlatformRuntime> CreateSdlPlatformRuntime();

}  // namespace platform
}  // namespace mocktail

#endif  // MOCKTAIL_PLATFORM_SDL_PLATFORM_RUNTIME_H_
