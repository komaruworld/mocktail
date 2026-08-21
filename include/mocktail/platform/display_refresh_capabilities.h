#ifndef MOCKTAIL_PLATFORM_DISPLAY_REFRESH_CAPABILITIES_H_
#define MOCKTAIL_PLATFORM_DISPLAY_REFRESH_CAPABILITIES_H_

#include <vector>

struct SDL_Window;

namespace mocktail {
namespace platform {

struct DisplayRefreshCapabilities {
  float current_hz = 0.0f;
  std::vector<float> supported_hz;

  bool valid() const { return current_hz > 0.0f; }
};

// Queries SDL video state. SDL requires this function to run on the thread
// that owns the window; callers may copy the returned immutable snapshot to
// worker-owned startup state afterwards.
DisplayRefreshCapabilities
QuerySdlDisplayRefreshCapabilities(SDL_Window *window);

// Normalizes raw display rates for deterministic publication and tests.
DisplayRefreshCapabilities
NormalizeDisplayRefreshCapabilities(float current_hz,
                                    std::vector<float> supported_hz);

} // namespace platform
} // namespace mocktail

#endif // MOCKTAIL_PLATFORM_DISPLAY_REFRESH_CAPABILITIES_H_
