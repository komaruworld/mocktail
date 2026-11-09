#include "window/window_surface_lifecycle.h"

#include <limits>
#include <utility>

#include "window/window_state_store.h"

namespace mocktail {
namespace window {
namespace {

Status InvalidArgument(std::string message) {
  return Status::Error(StatusCode::kInvalidArgument, std::move(message));
}

Status FailedPrecondition(std::string message) {
  return Status::Error(StatusCode::kFailedPrecondition, std::move(message));
}

bool ValidAvailableSurface(uintptr_t native_window, uint32_t width,
                           uint32_t height, float dpi_scale) {
  return native_window != 0 && width >= kMinimumWindowWidth &&
         height >= kMinimumWindowHeight && dpi_scale > 0.0f;
}

}  // namespace

Status WindowSurfaceLifecycle::Activate(uintptr_t native_window, uint32_t width,
                                        uint32_t height, float dpi_scale) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (active_) {
    return FailedPrecondition("window surface lifecycle is already active");
  }
  if (!ValidAvailableSurface(native_window, width, height, dpi_scale)) {
    return InvalidArgument(
        "initial window surface requires a native handle, dimensions and "
        "DPI scale");
  }
  events_.clear();
  current_ = {1, native_window, width, height, true, dpi_scale};
  active_ = true;
  return Status::Ok();
}

void WindowSurfaceLifecycle::Deactivate() {
  std::lock_guard<std::mutex> lock(mutex_);
  active_ = false;
  events_.clear();
  current_ = {};
}

Status WindowSurfaceLifecycle::Observe(uintptr_t native_window, uint32_t width,
                                       uint32_t height, float dpi_scale) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!active_) {
    return FailedPrecondition("window surface lifecycle is not active");
  }

  if (native_window == 0) {
    if (current_.available) {
      WindowSurfaceSnapshot destroyed = current_;
      destroyed.available = false;
      QueueLocked(WindowSurfaceEventType::kDestroyed, destroyed);
      current_.native_window = 0;
      current_.available = false;
    }
    return Status::Ok();
  }

  if (!(dpi_scale > 0.0f)) {
    return InvalidArgument("observed window surface requires a DPI scale");
  }

  // Compositors can report extents below SDL's minimum size during resize
  // and minimization. Retain the last usable extent: Roblox derives smaller
  // render targets from it, and a one-pixel axis produces a zero-sized texture.
  // The native surface lifetime is still tracked independently of its size.
  if (width < kMinimumWindowWidth || height < kMinimumWindowHeight) {
    if (current_.available && native_window != current_.native_window) {
      WindowSurfaceSnapshot destroyed = current_;
      destroyed.available = false;
      QueueLocked(WindowSurfaceEventType::kDestroyed, destroyed);
      current_.native_window = 0;
      current_.available = false;
    }
    return Status::Ok();
  }

  if (!current_.available) {
    return RestoreLocked(native_window, width, height, dpi_scale);
  }

  if (native_window != current_.native_window) {
    WindowSurfaceSnapshot destroyed = current_;
    destroyed.available = false;
    QueueLocked(WindowSurfaceEventType::kDestroyed, destroyed);
    current_.native_window = 0;
    current_.available = false;
    return RestoreLocked(native_window, width, height, dpi_scale);
  }

  if (width == current_.width && height == current_.height &&
      dpi_scale == current_.dpi_scale) {
    return Status::Ok();
  }
  current_.width = width;
  current_.height = height;
  current_.dpi_scale = dpi_scale;
  QueueLocked(WindowSurfaceEventType::kChanged, current_);
  return Status::Ok();
}

Status WindowSurfaceLifecycle::Refresh() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!active_) {
    return FailedPrecondition("window surface lifecycle is not active");
  }
  if (!current_.available ||
      !ValidAvailableSurface(current_.native_window, current_.width,
                             current_.height, current_.dpi_scale)) {
    return FailedPrecondition("window surface is unavailable for refresh");
  }
  QueueLocked(WindowSurfaceEventType::kChanged, current_);
  return Status::Ok();
}

Status WindowSurfaceLifecycle::Recreate() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!active_) {
    return FailedPrecondition("window surface lifecycle is not active");
  }
  if (!current_.available ||
      !ValidAvailableSurface(current_.native_window, current_.width,
                             current_.height, current_.dpi_scale)) {
    return FailedPrecondition("window surface is unavailable for recreation");
  }
  if (current_.generation == std::numeric_limits<uint64_t>::max()) {
    return FailedPrecondition("window surface generation overflowed");
  }

  WindowSurfaceSnapshot destroyed = current_;
  destroyed.available = false;
  QueueLocked(WindowSurfaceEventType::kDestroyed, destroyed);
  ++current_.generation;
  QueueLocked(WindowSurfaceEventType::kCreated, current_);
  QueueLocked(WindowSurfaceEventType::kChanged, current_);
  return Status::Ok();
}

bool WindowSurfaceLifecycle::Poll(WindowSurfaceEvent* event) {
  if (event == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (events_.empty()) {
    return false;
  }
  *event = events_.front();
  events_.pop_front();
  return true;
}

WindowSurfaceSnapshot WindowSurfaceLifecycle::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return current_;
}

bool WindowSurfaceLifecycle::active() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_;
}

void WindowSurfaceLifecycle::QueueLocked(WindowSurfaceEventType type,
                                         const WindowSurfaceSnapshot& surface) {
  if (type == WindowSurfaceEventType::kChanged && !events_.empty() &&
      events_.back().type == WindowSurfaceEventType::kChanged &&
      events_.back().surface.generation == surface.generation) {
    events_.back().surface = surface;
    return;
  }
  events_.push_back({type, surface});
}

Status WindowSurfaceLifecycle::RestoreLocked(uintptr_t native_window,
                                             uint32_t width, uint32_t height,
                                             float dpi_scale) {
  if (current_.generation == std::numeric_limits<uint64_t>::max()) {
    return FailedPrecondition("window surface generation overflowed");
  }
  ++current_.generation;
  current_.native_window = native_window;
  current_.width = width;
  current_.height = height;
  current_.dpi_scale = dpi_scale;
  current_.available = true;
  QueueLocked(WindowSurfaceEventType::kCreated, current_);
  QueueLocked(WindowSurfaceEventType::kChanged, current_);
  return Status::Ok();
}

const char* WindowSurfaceEventTypeName(WindowSurfaceEventType type) {
  switch (type) {
    case WindowSurfaceEventType::kCreated:
      return "Created";
    case WindowSurfaceEventType::kChanged:
      return "Changed";
    case WindowSurfaceEventType::kDestroyed:
      return "Destroyed";
  }
  return "Unknown";
}

}  // namespace window
}  // namespace mocktail
