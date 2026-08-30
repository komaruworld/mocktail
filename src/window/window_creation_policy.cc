#include "window/window_creation_policy.h"

namespace mocktail {
namespace window {

SDL_WindowFlags ApplyHighPixelDensityWindowFlag(SDL_WindowFlags flags,
                                                bool high_dpi) {
  if (high_dpi) {
    return flags | SDL_WINDOW_HIGH_PIXEL_DENSITY;
  }
  return flags & ~SDL_WINDOW_HIGH_PIXEL_DENSITY;
}

}  // namespace window
}  // namespace mocktail
