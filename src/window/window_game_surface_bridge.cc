#include "window/window_game_surface_bridge.h"

#include <utility>

#include "runtime/roblox_game_session_native_adapter.h"
#include "window/window.h"

namespace mocktail {
namespace window {
namespace {

Status InvalidArgument(std::string message) {
  return Status::Error(StatusCode::kInvalidArgument, std::move(message));
}

Status FailedPrecondition(std::string message) {
  return Status::Error(StatusCode::kFailedPrecondition, std::move(message));
}

runtime::GameSessionUpdateResult RuntimeCreated(void* context,
                                                uint64_t generation) {
  return static_cast<runtime::RobloxGameSessionRuntime*>(context)
      ->SurfaceCreated(generation);
}

runtime::GameSessionUpdateResult RuntimeChanged(void* context,
                                                runtime::GameSurface surface) {
  return static_cast<runtime::RobloxGameSessionRuntime*>(context)
      ->RebindSurface(surface);
}

runtime::GameSessionUpdateResult RuntimeDestroyed(void* context,
                                                  uint64_t generation) {
  return static_cast<runtime::RobloxGameSessionRuntime*>(context)
      ->SurfaceDestroyed(generation);
}

bool PollProductionWindowSurfaceEvent(void*, WindowSurfaceEvent* event) {
  return PollWindowSurfaceEvent(event);
}

Status RecordProductionResizeCommit(void*, const WindowSurfaceEvent& event) {
  return RecordResizeReadinessSurfaceCommit(event);
}

}  // namespace

WindowGameSurfaceConsumer MakeWindowGameSurfaceConsumer(
    runtime::RobloxGameSessionRuntime* runtime) {
  if (runtime == nullptr) {
    return {};
  }
  return {runtime, &RuntimeCreated, &RuntimeChanged, &RuntimeDestroyed};
}

WindowGameSurfaceEventSource MakeWindowGameSurfaceEventSource() {
  return {nullptr, &PollProductionWindowSurfaceEvent};
}

WindowGameSurfaceCommitObserver MakeWindowResizeReadinessCommitObserver() {
  return {nullptr, &RecordProductionResizeCommit};
}

WindowGameSurfaceBridge::WindowGameSurfaceBridge(
    WindowGameSurfaceEventSource source, WindowGameSurfaceConsumer consumer,
    WindowGameSurfaceCommitObserver observer)
    : source_(source), consumer_(consumer), observer_(observer) {
  if (!source_.complete()) {
    terminal_status_ =
        InvalidArgument("window game-surface event source is incomplete");
    snapshot_.failed = true;
  } else if (!consumer_.complete()) {
    terminal_status_ =
        InvalidArgument("window game-surface consumer is incomplete");
    snapshot_.failed = true;
  }
}

WindowGameSurfaceDrainResult WindowGameSurfaceBridge::Drain() {
  if (!terminal_status_.ok()) {
    return {terminal_status_, 0};
  }

  std::size_t drained_events = 0;
  WindowSurfaceEvent event;
  while (source_.poll(source_.context, &event)) {
    ++drained_events;
    Status validation = ValidateEvent(event);
    if (!validation.ok()) {
      return Fail(std::move(validation), drained_events);
    }

    const runtime::GameSessionUpdateResult update = Dispatch(event);
    if (!update.ok()) {
      const std::string detail = update.cause.ok()
                                     ? "GAME lifecycle rejected surface event"
                                     : update.cause.message();
      return Fail(FailedPrecondition(detail), drained_events);
    }
    if (observer_.complete()) {
      Status observe_status = observer_.committed(observer_.context, event);
      if (!observe_status.ok()) {
        return Fail(std::move(observe_status), drained_events);
      }
    }

    snapshot_.last_event_type = event.type;
    snapshot_.last_generation = event.surface.generation;
    if (update.changed()) {
      ++snapshot_.accepted_events;
    } else {
      ++snapshot_.duplicate_events;
    }
  }
  return {Status::Ok(), drained_events};
}

WindowGameSurfaceBridgeSnapshot WindowGameSurfaceBridge::Snapshot() const {
  return snapshot_;
}

Status WindowGameSurfaceBridge::ValidateEvent(
    const WindowSurfaceEvent& event) const {
  if (event.surface.generation == 0) {
    return InvalidArgument("window surface event has no generation");
  }
  switch (event.type) {
    case WindowSurfaceEventType::kCreated:
      return Status::Ok();
    case WindowSurfaceEventType::kChanged:
      if (!event.surface.available || event.surface.native_window == 0 ||
          event.surface.width == 0 || event.surface.height == 0) {
        return InvalidArgument(
            "changed window surface requires an available handle and extent");
      }
      return Status::Ok();
    case WindowSurfaceEventType::kDestroyed:
      return Status::Ok();
  }
  return InvalidArgument("window surface event type is invalid");
}

runtime::GameSessionUpdateResult WindowGameSurfaceBridge::Dispatch(
    const WindowSurfaceEvent& event) const {
  switch (event.type) {
    case WindowSurfaceEventType::kCreated:
      return consumer_.created(consumer_.context, event.surface.generation);
    case WindowSurfaceEventType::kChanged:
      return consumer_.changed(
          consumer_.context,
          {event.surface.generation, event.surface.native_window,
           event.surface.width, event.surface.height});
    case WindowSurfaceEventType::kDestroyed:
      return consumer_.destroyed(consumer_.context, event.surface.generation);
  }
  runtime::GameSessionUpdateResult rejected;
  rejected.cause = InvalidArgument("window surface event type is invalid");
  return rejected;
}

WindowGameSurfaceDrainResult WindowGameSurfaceBridge::Fail(
    Status status, std::size_t drained_events) {
  terminal_status_ = std::move(status);
  snapshot_.failed = true;
  return {terminal_status_, drained_events};
}

}  // namespace window
}  // namespace mocktail
