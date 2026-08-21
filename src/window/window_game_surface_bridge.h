#ifndef MOCKTAIL_WINDOW_WINDOW_GAME_SURFACE_BRIDGE_H_
#define MOCKTAIL_WINDOW_WINDOW_GAME_SURFACE_BRIDGE_H_

#include <cstddef>
#include <cstdint>

#include "mocktail/status.h"
#include "runtime/game_session_coordinator.h"
#include "window/window_surface_lifecycle.h"

namespace mocktail {
namespace runtime {
class RobloxGameSessionRuntime;
}  // namespace runtime

namespace window {

using PollWindowGameSurfaceEventFn = bool (*)(
    void* context, WindowSurfaceEvent* event);

struct WindowGameSurfaceEventSource {
  void* context = nullptr;
  PollWindowGameSurfaceEventFn poll = nullptr;

  bool complete() const { return poll != nullptr; }
};

using GameSurfaceCreatedFn = runtime::GameSessionUpdateResult (*)(
    void* context, uint64_t generation);
using GameSurfaceChangedFn = runtime::GameSessionUpdateResult (*)(
    void* context, runtime::GameSurface surface);
using GameSurfaceDestroyedFn = runtime::GameSessionUpdateResult (*)(
    void* context, uint64_t generation);

struct WindowGameSurfaceConsumer {
  void* context = nullptr;
  GameSurfaceCreatedFn created = nullptr;
  GameSurfaceChangedFn changed = nullptr;
  GameSurfaceDestroyedFn destroyed = nullptr;

  bool complete() const {
    return context != nullptr && created != nullptr && changed != nullptr &&
           destroyed != nullptr;
  }
};

using CommittedWindowGameSurfaceEventFn = Status (*)(
    void* context, const WindowSurfaceEvent& event);

struct WindowGameSurfaceCommitObserver {
  void* context = nullptr;
  CommittedWindowGameSurfaceEventFn committed = nullptr;

  bool complete() const { return committed != nullptr; }
};

WindowGameSurfaceConsumer MakeWindowGameSurfaceConsumer(
    runtime::RobloxGameSessionRuntime* runtime);
WindowGameSurfaceEventSource MakeWindowGameSurfaceEventSource();
WindowGameSurfaceCommitObserver MakeWindowResizeReadinessCommitObserver();

struct WindowGameSurfaceBridgeSnapshot {
  uint64_t accepted_events = 0;
  uint64_t duplicate_events = 0;
  uint64_t last_generation = 0;
  WindowSurfaceEventType last_event_type = WindowSurfaceEventType::kChanged;
  bool failed = false;
};

struct WindowGameSurfaceDrainResult {
  Status status = Status::Ok();
  std::size_t drained_events = 0;

  bool ok() const { return status.ok(); }
};

// Owns the translation between host window events and the Roblox GAME
// lifecycle. Drain is intentionally synchronous: each polled event is
// dispatched before the next event can be removed from the source. After any
// rejection the bridge becomes terminal and never consumes a later event,
// preserving the first actionable failure.
class WindowGameSurfaceBridge final {
 public:
  WindowGameSurfaceBridge(WindowGameSurfaceEventSource source,
                          WindowGameSurfaceConsumer consumer,
                          WindowGameSurfaceCommitObserver observer = {});

  WindowGameSurfaceBridge(const WindowGameSurfaceBridge&) = delete;
  WindowGameSurfaceBridge& operator=(const WindowGameSurfaceBridge&) = delete;

  WindowGameSurfaceDrainResult Drain();
  WindowGameSurfaceBridgeSnapshot Snapshot() const;

 private:
  Status ValidateEvent(const WindowSurfaceEvent& event) const;
  runtime::GameSessionUpdateResult Dispatch(
      const WindowSurfaceEvent& event) const;
  WindowGameSurfaceDrainResult Fail(Status status,
                                    std::size_t drained_events);

  const WindowGameSurfaceEventSource source_;
  const WindowGameSurfaceConsumer consumer_;
  const WindowGameSurfaceCommitObserver observer_;
  WindowGameSurfaceBridgeSnapshot snapshot_;
  Status terminal_status_ = Status::Ok();
};

}  // namespace window
}  // namespace mocktail

#endif  // MOCKTAIL_WINDOW_WINDOW_GAME_SURFACE_BRIDGE_H_
