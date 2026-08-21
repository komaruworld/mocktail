#ifndef MOCKTAIL_WINDOW_WINDOW_STATE_STORE_H_
#define MOCKTAIL_WINDOW_WINDOW_STATE_STORE_H_

#include <filesystem>

#include "mocktail/status.h"

namespace mocktail {
namespace window {

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
};

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
