#ifndef MOCKTAIL_WINDOW_WINDOW_SURFACE_LIFECYCLE_H_
#define MOCKTAIL_WINDOW_WINDOW_SURFACE_LIFECYCLE_H_

#include <cstdint>
#include <deque>
#include <mutex>

#include "mocktail/status.h"

namespace mocktail {
namespace window {

enum class WindowSurfaceEventType {
  kCreated,
  kChanged,
  kDestroyed,
};

struct WindowSurfaceSnapshot {
  uint64_t generation = 0;
  uintptr_t native_window = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  bool available = false;
};

struct WindowSurfaceEvent {
  WindowSurfaceEventType type = WindowSurfaceEventType::kChanged;
  WindowSurfaceSnapshot surface;
};

// Converts observed host-window facts into Android-style surface lifetimes.
// Positive-size changes on the same native handle keep the generation. A new
// generation is created only after an observed handle replacement or a real
// handle loss followed by restoration.
class WindowSurfaceLifecycle final {
 public:
  Status Activate(uintptr_t native_window, uint32_t width, uint32_t height);
  void Deactivate();
  Status Observe(uintptr_t native_window, uint32_t width, uint32_t height);
  // Queues an authoritative same-generation Changed event. This is used when
  // a compositor changes presentation state (for example entering fullscreen)
  // without changing the final native handle or pixel extent.
  Status Refresh();
  // Replays the Android surface-destruction contract around the current host
  // window. Vulkan uses this after a real WSI out-of-date result because a
  // same-generation Changed event can be ignored by the guest once its
  // swapchain has retained an obsolete compositor surface.
  Status Recreate();
  bool Poll(WindowSurfaceEvent* event);
  WindowSurfaceSnapshot Snapshot() const;
  bool active() const;

 private:
  void QueueLocked(WindowSurfaceEventType type,
                   const WindowSurfaceSnapshot& surface);
  Status RestoreLocked(uintptr_t native_window, uint32_t width,
                       uint32_t height);

  mutable std::mutex mutex_;
  std::deque<WindowSurfaceEvent> events_;
  WindowSurfaceSnapshot current_;
  bool active_ = false;
};

const char* WindowSurfaceEventTypeName(WindowSurfaceEventType type);

}  // namespace window
}  // namespace mocktail

#endif  // MOCKTAIL_WINDOW_WINDOW_SURFACE_LIFECYCLE_H_
