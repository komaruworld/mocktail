#ifndef MOCKTAIL_WINDOW_WINDOW_CREATION_POLICY_H_
#define MOCKTAIL_WINDOW_WINDOW_CREATION_POLICY_H_

#include <SDL3/SDL_video.h>

namespace mocktail {
namespace window {

SDL_WindowFlags ApplyHighPixelDensityWindowFlag(SDL_WindowFlags flags,
                                                bool high_dpi);

}  // namespace window
}  // namespace mocktail

#endif  // MOCKTAIL_WINDOW_WINDOW_CREATION_POLICY_H_
