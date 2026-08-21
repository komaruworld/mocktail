#include "window/window_resize_readiness_gate.h"

#include <utility>

namespace mocktail {
namespace window {
namespace {

Status InvalidArgument(std::string message) {
  return Status::Error(StatusCode::kInvalidArgument, std::move(message));
}

Status FailedPrecondition(std::string message) {
  return Status::Error(StatusCode::kFailedPrecondition, std::move(message));
}

}  // namespace

Status WindowResizeReadinessGate::Activate(
    WindowResizeReadinessConfig config,
    const WindowSurfaceSnapshot& initial_surface) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (snapshot_.state != WindowResizeReadinessState::kDisabled ||
      snapshot_.surface_generation != 0) {
    return FailedPrecondition("window resize readiness gate is already active");
  }
  if (!config.enabled) {
    return Status::Ok();
  }
  if (!initial_surface.available || initial_surface.generation == 0 ||
      initial_surface.native_window == 0 || initial_surface.width == 0 ||
      initial_surface.height == 0) {
    return InvalidArgument(
        "window resize readiness gate requires an available initial surface");
  }
  if (config.target_width == 0 || config.target_height == 0) {
    return InvalidArgument("window resize target must have a positive extent");
  }
  if (config.target_width == initial_surface.width &&
      config.target_height == initial_surface.height) {
    return InvalidArgument("window resize target must differ from initial extent");
  }
  snapshot_.state = WindowResizeReadinessState::kWaitingForFirstPresent;
  snapshot_.surface_generation = initial_surface.generation;
  snapshot_.target_width = config.target_width;
  snapshot_.target_height = config.target_height;
  return Status::Ok();
}

Status WindowResizeReadinessGate::RecordPresent(uint64_t frame_serial) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (snapshot_.state == WindowResizeReadinessState::kDisabled) {
    return Status::Ok();
  }
  if (snapshot_.state == WindowResizeReadinessState::kFailed ||
      snapshot_.state == WindowResizeReadinessState::kStopped) {
    return FailedPrecondition(
        snapshot_.failure_reason.empty()
            ? "window resize readiness gate cannot accept a present"
            : snapshot_.failure_reason);
  }
  if (frame_serial == 0) {
    return FailLocked("resize readiness received a zero present serial");
  }
  if (snapshot_.state ==
      WindowResizeReadinessState::kWaitingForFirstPresent) {
    snapshot_.first_present_serial = frame_serial;
    snapshot_.state = WindowResizeReadinessState::kReadyToRequestResize;
    return Status::Ok();
  }
  if (snapshot_.state == WindowResizeReadinessState::kSurfaceCommitted) {
    if (frame_serial <= snapshot_.first_present_serial) {
      return FailLocked(
          "post-rebind present serial does not follow the first present");
    }
    snapshot_.post_rebind_present_serial = frame_serial;
    snapshot_.state = WindowResizeReadinessState::kPostRebindPresented;
  }
  return Status::Ok();
}

bool WindowResizeReadinessGate::TakeResizeRequest(
    WindowResizeRequest* request) {
  if (request == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  if (snapshot_.state != WindowResizeReadinessState::kReadyToRequestResize) {
    return false;
  }
  request->width = snapshot_.target_width;
  request->height = snapshot_.target_height;
  snapshot_.state = WindowResizeReadinessState::kResizeRequested;
  return true;
}

Status WindowResizeReadinessGate::RecordCommittedSurfaceEvent(
    const WindowSurfaceEvent& event) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (snapshot_.state == WindowResizeReadinessState::kDisabled) {
    return Status::Ok();
  }
  if (snapshot_.state == WindowResizeReadinessState::kFailed ||
      snapshot_.state == WindowResizeReadinessState::kStopped) {
    return FailedPrecondition(
        snapshot_.failure_reason.empty()
            ? "window resize readiness gate cannot accept a surface event"
            : snapshot_.failure_reason);
  }
  if (snapshot_.state != WindowResizeReadinessState::kResizeRequested) {
    return Status::Ok();
  }
  if (event.type == WindowSurfaceEventType::kCreated ||
      event.type == WindowSurfaceEventType::kDestroyed) {
    return FailLocked(
        "resize recreated the surface instead of preserving its generation");
  }
  if (event.surface.generation != snapshot_.surface_generation) {
    return FailLocked("resize committed a different surface generation");
  }
  if (!event.surface.available || event.surface.native_window == 0) {
    return FailLocked("resize committed an unavailable surface");
  }
  if (event.surface.width != snapshot_.target_width ||
      event.surface.height != snapshot_.target_height) {
    // Window managers may emit intermediate configure events. The exact
    // compositor-confirmed target remains the only readiness evidence.
    return Status::Ok();
  }
  snapshot_.state = WindowResizeReadinessState::kSurfaceCommitted;
  return Status::Ok();
}

Status WindowResizeReadinessGate::RecordStopped() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (snapshot_.state == WindowResizeReadinessState::kDisabled) {
    return Status::Ok();
  }
  if (snapshot_.state == WindowResizeReadinessState::kFailed) {
    return FailedPrecondition(snapshot_.failure_reason);
  }
  if (snapshot_.state !=
      WindowResizeReadinessState::kPostRebindPresented) {
    return FailLocked(
        "runtime stopped before resize rebind produced a later present");
  }
  snapshot_.state = WindowResizeReadinessState::kStopped;
  return Status::Ok();
}

Status WindowResizeReadinessGate::CompletionStatus() const {
  std::lock_guard<std::mutex> lock(mutex_);
  if (snapshot_.state == WindowResizeReadinessState::kDisabled ||
      snapshot_.state == WindowResizeReadinessState::kStopped) {
    return Status::Ok();
  }
  if (snapshot_.state == WindowResizeReadinessState::kFailed) {
    return FailedPrecondition(snapshot_.failure_reason);
  }
  return FailedPrecondition(
      "window resize readiness evidence is incomplete");
}

WindowResizeReadinessSnapshot WindowResizeReadinessGate::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return snapshot_;
}

Status WindowResizeReadinessGate::FailLocked(std::string reason) {
  snapshot_.state = WindowResizeReadinessState::kFailed;
  snapshot_.failure_reason = std::move(reason);
  return FailedPrecondition(snapshot_.failure_reason);
}

const char* WindowResizeReadinessStateName(WindowResizeReadinessState state) {
  switch (state) {
    case WindowResizeReadinessState::kDisabled:
      return "Disabled";
    case WindowResizeReadinessState::kWaitingForFirstPresent:
      return "WaitingForFirstPresent";
    case WindowResizeReadinessState::kReadyToRequestResize:
      return "ReadyToRequestResize";
    case WindowResizeReadinessState::kResizeRequested:
      return "ResizeRequested";
    case WindowResizeReadinessState::kSurfaceCommitted:
      return "SurfaceCommitted";
    case WindowResizeReadinessState::kPostRebindPresented:
      return "PostRebindPresented";
    case WindowResizeReadinessState::kStopped:
      return "Stopped";
    case WindowResizeReadinessState::kFailed:
      return "Failed";
  }
  return "Unknown";
}

}  // namespace window
}  // namespace mocktail
