#ifndef MOCKTAIL_WINDOW_WINDOW_RESIZE_READINESS_GATE_H_
#define MOCKTAIL_WINDOW_WINDOW_RESIZE_READINESS_GATE_H_

#include <cstdint>
#include <mutex>
#include <string>

#include "mocktail/status.h"
#include "window/window_surface_lifecycle.h"

namespace mocktail {
namespace window {

enum class WindowResizeReadinessState {
  kDisabled,
  kWaitingForFirstPresent,
  kReadyToRequestResize,
  kResizeRequested,
  kSurfaceCommitted,
  kPostRebindPresented,
  kStopped,
  kFailed,
};

struct WindowResizeReadinessConfig {
  bool enabled = false;
  uint32_t target_width = 0;
  uint32_t target_height = 0;
};

struct WindowResizeRequest {
  uint32_t width = 0;
  uint32_t height = 0;
};

struct WindowResizeReadinessSnapshot {
  WindowResizeReadinessState state = WindowResizeReadinessState::kDisabled;
  uint64_t surface_generation = 0;
  uint64_t first_present_serial = 0;
  uint64_t post_rebind_present_serial = 0;
  uint32_t target_width = 0;
  uint32_t target_height = 0;
  std::string failure_reason;

  bool complete() const {
    return state == WindowResizeReadinessState::kDisabled ||
           state == WindowResizeReadinessState::kStopped;
  }
};

// Strict evidence state machine for the opt-in real resize gate. It never
// creates a surface event. Production must call TakeResizeRequest after a real
// present, pass the request to SDL_SetWindowSize, and report only compositor-
// observed events that were committed by the GAME/JNI bridge.
class WindowResizeReadinessGate final {
 public:
  Status Activate(WindowResizeReadinessConfig config,
                  const WindowSurfaceSnapshot& initial_surface);
  Status RecordPresent(uint64_t frame_serial);
  bool TakeResizeRequest(WindowResizeRequest* request);
  Status RecordCommittedSurfaceEvent(const WindowSurfaceEvent& event);
  Status RecordStopped();
  Status CompletionStatus() const;
  WindowResizeReadinessSnapshot Snapshot() const;

 private:
  Status FailLocked(std::string reason);

  mutable std::mutex mutex_;
  WindowResizeReadinessSnapshot snapshot_;
};

const char* WindowResizeReadinessStateName(WindowResizeReadinessState state);

}  // namespace window
}  // namespace mocktail

#endif  // MOCKTAIL_WINDOW_WINDOW_RESIZE_READINESS_GATE_H_
