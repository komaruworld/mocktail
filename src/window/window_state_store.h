#ifndef MOCKTAIL_WINDOW_WINDOW_STATE_STORE_H_
#define MOCKTAIL_WINDOW_WINDOW_STATE_STORE_H_

#include <filesystem>

#include "mocktail/status.h"

namespace mocktail {
namespace window {

// Smallest window the renderer is expected to cope with. A restored geometry
// below this is rejected, and the live window is floored at the same size.
inline constexpr int kMinimumWindowWidth = 160;
inline constexpr int kMinimumWindowHeight = 120;

// Windowed geometry remains separate from fullscreen/maximized state so a
// compositor transition cannot replace the useful restore rectangle with the
// monitor-sized surface extent.
struct PersistedWindowState {
  int x = 0;
  int y = 0;
  int width = 1280;
  int height = 720;
  bool has_position = false;
  bool fullscreen = false;
  bool maximized = false;
  int configured_width = 0;
  int configured_height = 0;
  bool has_configured_size = false;
};

inline bool RestoredSizeApplies(const PersistedWindowState& state,
                                int configured_width, int configured_height) {
  return state.has_configured_size &&
         state.configured_width == configured_width &&
         state.configured_height == configured_height;
}

// Fullscreen can be part of native window creation, but maximization must be
// requested only after startup size constraints and the windowed restore
// position have been applied. On X11, changing the minimum size of a window
// created with SDL_WINDOW_MAXIMIZED can synchronously wait on that initial
// maximize transition and crash or hang before the first event pump.
struct WindowStartupPresentationPlan {
  bool fullscreen_at_creation = false;
  bool maximize_after_constraints = false;
};

WindowStartupPresentationPlan PlanWindowStartupPresentation(
    const PersistedWindowState& state);

struct WindowStateLoadResult {
  bool found = false;
  PersistedWindowState state;
  Status status;

  explicit operator bool() const { return status.ok(); }
};

// Reads and writes the bounded, versioned host window state. The writer uses
// a same-directory temporary file, fsync, and rename so an interrupted launch
// cannot leave a partially written restore record.
WindowStateLoadResult LoadWindowState(const std::filesystem::path& path);
Status StoreWindowState(const std::filesystem::path& path,
                        const PersistedWindowState& state);

}  // namespace window
}  // namespace mocktail

#endif  // MOCKTAIL_WINDOW_WINDOW_STATE_STORE_H_
