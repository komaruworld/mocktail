#include "runtime/game_session_coordinator.h"

#include <utility>

namespace mocktail {
namespace runtime {
namespace {

Status InvalidArgument(std::string message) {
  return Status::Error(StatusCode::kInvalidArgument, std::move(message));
}

Status FailedPrecondition(std::string message) {
  return Status::Error(StatusCode::kFailedPrecondition, std::move(message));
}

bool ValidPrincipal(const GameSessionPrincipal& principal) {
  if (principal.generation == 0 || principal.base_url.empty()) {
    return false;
  }
  if (principal.kind == GameSessionPrincipalKind::kAuthenticated) {
    return !principal.principal_id.empty();
  }
  return principal.kind == GameSessionPrincipalKind::kLocalGuest &&
         principal.principal_id.empty();
}

bool ValidRequest(const GameJoinRequest& request) {
  return request.request_id != 0 && request.place_id >= 0 &&
         !request.launch_parameters.empty();
}

bool ValidSurface(const GameSurface& surface) {
  return surface.generation != 0 && surface.native_window != 0 &&
         surface.width != 0 && surface.height != 0;
}

}  // namespace

bool GameSessionCapabilities::complete() const {
  return set_game_foreground != nullptr && start_game != nullptr &&
         update_surface != nullptr && pause_game != nullptr &&
         resume_game != nullptr && leave_game != nullptr &&
         shutdown_runtime != nullptr;
}

GameSessionCoordinator::GameSessionCoordinator(
    GameSessionCapabilities capabilities, GameSessionRequirements requirements)
    : capabilities_(std::move(capabilities)), requirements_(requirements) {}

GameSessionUpdateResult GameSessionCoordinator::SetPrincipal(
    GameSessionPrincipal principal) {
  std::lock_guard<std::mutex> operation_lock(operation_mutex_);
  std::lock_guard<std::mutex> lock(mutex_);
  if (principal_ready_) {
    if (SamePrincipal(principal_, principal)) {
      return DuplicateLocked("game-session principal already accepted");
    }
    if (IsForwardTerminalLocked()) {
      return RejectTerminalLocked("replace game-session principal");
    }
    return FailLocked(
        FailedPrecondition("game-session principal replacement is forbidden"),
        "conflicting game-session principal");
  }
  if (IsForwardTerminalLocked()) {
    return RejectTerminalLocked("set game-session principal");
  }
  if (state_ != GameSessionState::kCreated) {
    return FailLocked(FailedPrecondition("principal must be set first"),
                      "invalid principal order");
  }
  if (!capabilities_.complete()) {
    return FailLocked(
        Status::Error(StatusCode::kUnavailable,
                      "game-session capability table is incomplete"),
        "cannot accept principal without complete native capabilities");
  }
  if (!ValidPrincipal(principal)) {
    return FailLocked(
        InvalidArgument("principal generation and base URL are required; "
                        "authenticated principals also require an ID"),
        "invalid game-session principal");
  }

  principal_ = std::move(principal);
  principal_ready_ = true;
  return ApplyLocked(GameSessionState::kPrincipalReady,
                     "game-session principal accepted");
}

GameSessionUpdateResult GameSessionCoordinator::QueueJoinRequest(
    GameJoinRequest request) {
  std::lock_guard<std::mutex> operation_lock(operation_mutex_);
  std::unique_lock<std::mutex> lock(mutex_);
  if (join_request_queued_) {
    if (SameRequest(request_, request)) {
      return DuplicateLocked("game join request already queued");
    }
    if (IsForwardTerminalLocked()) {
      return RejectTerminalLocked("replace game join request");
    }
    return FailLocked(
        FailedPrecondition("queued game join request is immutable"),
        "conflicting game join request");
  }
  if (IsForwardTerminalLocked()) {
    return RejectTerminalLocked("queue game join request");
  }
  if (!principal_ready_) {
    return FailLocked(
        FailedPrecondition("game-session principal is required before join"),
        "game join request arrived before principal");
  }
  if (!ValidRequest(request)) {
    return FailLocked(
        InvalidArgument("request ID and launch parameters are required"),
        "invalid game join request");
  }
  if (request.place_id != 0 &&
      principal_.kind != GameSessionPrincipalKind::kAuthenticated) {
    return FailLocked(
        FailedPrecondition(
            "authenticated principal is required for a network game join"),
        "local guest attempted a non-local game join");
  }

  request_ = std::move(request);
  join_request_queued_ = true;
  if (!pending_surface_available_) {
    return ApplyLocked(GameSessionState::kJoinRequestQueued,
                       "game join request queued; waiting for changed surface");
  }
  return StartIfReadyLocked(&lock,
                            "game join request completed start prerequisites");
}

GameSessionUpdateResult GameSessionCoordinator::SurfaceCreated(
    uint64_t surface_generation) {
  std::lock_guard<std::mutex> operation_lock(operation_mutex_);
  std::unique_lock<std::mutex> lock(mutex_);
  if (surface_created_ && surface_generation != 0 &&
      surface_generation == prepared_surface_generation_) {
    return DuplicateLocked("surface generation is already prepared");
  }
  if (IsForwardTerminalLocked()) {
    return RejectTerminalLocked("create game surface");
  }
  if (!principal_ready_) {
    return FailLocked(
        FailedPrecondition(
            "game-session principal is required before surface events"),
        "SurfaceCreated arrived before principal");
  }
  if (surface_generation == 0) {
    return FailLocked(InvalidArgument("surface generation is required"),
                      "invalid created surface");
  }
  if (initial_start_completed_) {
    if (state_ != GameSessionState::kPaused) {
      return FailLocked(
          FailedPrecondition(
              "a new surface can be prepared only after the game pauses"),
          "SurfaceCreated arrived while game was not paused");
    }
    if (surface_generation == active_surface_.generation ||
        surface_generation == last_destroyed_surface_generation_) {
      return FailLocked(
          InvalidArgument(
              "recreated surface requires a new lifetime generation"),
          "paused game received stale SurfaceCreated generation");
    }
  } else if (surface_created_) {
    return FailLocked(
        FailedPrecondition(
            "current prepared surface must be destroyed before replacement"),
        "conflicting SurfaceCreated generation");
  }

  state_ = GameSessionState::kPreparingSurface;
  ++revision_;
  lock.unlock();
  const Status status =
      capabilities_.set_game_foreground(capabilities_.context);
  lock.lock();
  if (!status.ok()) {
    return FailLocked(status, "ES.onSurfaceCreated foreground callback failed");
  }
  surface_created_ = true;
  prepared_surface_generation_ = surface_generation;
  return ApplyLocked(initial_start_completed_ ? GameSessionState::kPaused
                                              : PreStartStateLocked(),
                     "surface generation prepared and game foregrounded");
}

GameSessionUpdateResult GameSessionCoordinator::SurfaceChanged(
    GameSurface surface) {
  return UpdateSurface(std::move(surface), false);
}

GameSessionUpdateResult GameSessionCoordinator::RebindSurface(
    GameSurface surface) {
  return UpdateSurface(std::move(surface), true);
}

GameSessionUpdateResult GameSessionCoordinator::UpdateSurface(
    GameSurface surface, bool force_rebind) {
  std::lock_guard<std::mutex> operation_lock(operation_mutex_);
  std::unique_lock<std::mutex> lock(mutex_);
  if (IsForwardTerminalLocked()) {
    return RejectTerminalLocked("change game surface");
  }
  if (!principal_ready_) {
    return FailLocked(
        FailedPrecondition(
            "game-session principal is required before surface events"),
        "SurfaceChanged arrived before principal");
  }
  if (!ValidSurface(surface)) {
    return FailLocked(
        InvalidArgument(
            "surface generation, native window and dimensions are required"),
        "invalid changed surface");
  }
  if (!surface_created_ || surface.generation != prepared_surface_generation_) {
    return FailLocked(
        FailedPrecondition(
            "SurfaceChanged requires a matching successful SurfaceCreated"),
        "unprepared SurfaceChanged generation");
  }

  if (!initial_start_completed_) {
    if (pending_surface_available_ && SameSurface(pending_surface_, surface)) {
      return DuplicateLocked("initial candidate surface already current");
    }
    pending_surface_ = surface;
    pending_surface_available_ = true;
    if (!join_request_queued_) {
      return ApplyLocked(GameSessionState::kSurfaceAvailable,
                         "changed surface accepted; waiting for join request");
    }
    return StartIfReadyLocked(&lock,
                              "changed surface completed start prerequisites");
  }

  if (state_ == GameSessionState::kPaused) {
    pending_surface_ = surface;
    pending_surface_available_ = true;
    DiscardPendingPresentLocked();
    state_ = GameSessionState::kResuming;
    ++revision_;
    lock.unlock();
    const Status status =
        capabilities_.resume_game(capabilities_.context, surface);
    lock.lock();
    if (!status.ok()) {
      DiscardPendingSurfaceLocked();
      DiscardPendingPresentLocked();
      return FailLocked(status, "ResumeGame callback failed");
    }
    PromotePendingSurfaceLocked();
    game_paused_ = false;
    CommitPendingPresentLocked();
    return ApplyLocked(GameSessionState::kRunning,
                       "game resumed with recreated surface");
  }

  if (state_ != GameSessionState::kRunning) {
    return FailLocked(
        FailedPrecondition("surface update requires a running game"),
        "invalid SurfaceChanged order");
  }
  if (!force_rebind && active_surface_available_ &&
      SameSurface(active_surface_, surface)) {
    return DuplicateLocked("running game surface already current");
  }

  pending_surface_ = surface;
  pending_surface_available_ = true;
  DiscardPendingPresentLocked();
  state_ = GameSessionState::kUpdatingSurface;
  ++revision_;
  lock.unlock();
  const Status status =
      capabilities_.update_surface(capabilities_.context, surface);
  lock.lock();
  if (!status.ok()) {
    DiscardPendingSurfaceLocked();
    DiscardPendingPresentLocked();
    return FailLocked(status, "UpdateSurfaceGame callback failed");
  }
  PromotePendingSurfaceLocked();
  CommitPendingPresentLocked();
  return ApplyLocked(GameSessionState::kRunning,
                     "running game surface updated");
}

GameSessionUpdateResult GameSessionCoordinator::SurfaceDestroyed(
    uint64_t surface_generation) {
  std::lock_guard<std::mutex> operation_lock(operation_mutex_);
  std::unique_lock<std::mutex> lock(mutex_);
  if (!surface_created_ && !pending_surface_available_ &&
      !active_surface_available_ && surface_generation != 0 &&
      surface_generation == last_destroyed_surface_generation_) {
    return DuplicateLocked("surface generation is already destroyed");
  }
  if (IsForwardTerminalLocked()) {
    return RejectTerminalLocked("destroy game surface");
  }
  if (surface_generation == 0) {
    return FailLocked(InvalidArgument("surface generation is required"),
                      "invalid destroyed surface");
  }

  if (!initial_start_completed_) {
    const bool matches_prepared =
        surface_created_ && surface_generation == prepared_surface_generation_;
    const bool matches_candidate =
        pending_surface_available_ &&
        surface_generation == pending_surface_.generation;
    if (!matches_prepared && !matches_candidate) {
      return FailLocked(
          FailedPrecondition("destroyed surface was never prepared"),
          "unknown pre-start SurfaceDestroyed generation");
    }
    surface_created_ = false;
    prepared_surface_generation_ = 0;
    last_destroyed_surface_generation_ = surface_generation;
    DiscardPendingSurfaceLocked();
    DiscardPendingPresentLocked();
    return ApplyLocked(PreStartStateLocked(),
                       "unused pre-start surface destroyed");
  }

  if (state_ != GameSessionState::kRunning || !active_surface_available_) {
    return FailLocked(
        FailedPrecondition("SurfaceDestroyed requires an active running game"),
        "invalid SurfaceDestroyed order");
  }
  if (surface_generation != active_surface_.generation) {
    return FailLocked(
        FailedPrecondition("destroyed surface is not the active surface"),
        "conflicting SurfaceDestroyed generation");
  }

  // Host surface destruction is a fact before the guest PauseGame callback.
  // A failed callback must never resurrect an invalid native window.
  active_surface_available_ = false;
  surface_created_ = false;
  prepared_surface_generation_ = 0;
  last_destroyed_surface_generation_ = surface_generation;
  DiscardPendingSurfaceLocked();
  DiscardPendingPresentLocked();
  state_ = GameSessionState::kPausing;
  ++revision_;
  lock.unlock();
  const Status status = capabilities_.pause_game(capabilities_.context);
  lock.lock();
  if (!status.ok()) {
    return FailLocked(status, "PauseGame callback failed");
  }
  game_paused_ = true;
  return ApplyLocked(GameSessionState::kPaused,
                     "game paused after surface destruction");
}

GameSessionUpdateResult GameSessionCoordinator::RecordGamePresent(
    GamePresentEvidence evidence) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!IsPresentShapeValidLocked(evidence)) {
    return RejectLocked(
        InvalidArgument(
            "present must succeed with a serial and required backend"),
        "malformed or unsupported game present evidence");
  }

  const bool matches_pending =
      pending_surface_available_ &&
      evidence.surface_generation == pending_surface_.generation;
  const bool matches_active =
      active_surface_.generation != 0 &&
      evidence.surface_generation == active_surface_.generation;

  bool defer_until_native_success = false;
  bool immediate_observation = false;
  switch (state_) {
    case GameSessionState::kStarting:
    case GameSessionState::kResuming:
      defer_until_native_success = matches_pending;
      break;
    case GameSessionState::kUpdatingSurface:
      // A same-generation resize is still pending until UpdateSurface succeeds.
      defer_until_native_success = matches_pending;
      immediate_observation = !defer_until_native_success && matches_active &&
                              active_surface_available_;
      break;
    case GameSessionState::kRunning:
      immediate_observation = matches_active && active_surface_available_;
      break;
    case GameSessionState::kPausing:
    case GameSessionState::kPaused:
      // A queued render-thread present can arrive after host destruction. It is
      // valid historical evidence for the last active surface, not a
      // transition.
      immediate_observation = matches_active;
      break;
    case GameSessionState::kCreated:
    case GameSessionState::kPrincipalReady:
    case GameSessionState::kJoinRequestQueued:
    case GameSessionState::kPreparingSurface:
    case GameSessionState::kSurfaceCreated:
    case GameSessionState::kSurfaceAvailable:
    case GameSessionState::kLeaving:
    case GameSessionState::kReturnedToLuaApp:
    case GameSessionState::kFailure:
    case GameSessionState::kStopping:
    case GameSessionState::kStopped:
      break;
  }

  if (!defer_until_native_success && !immediate_observation) {
    return RejectLocked(
        FailedPrecondition(
            "present evidence does not match a current or pending surface"),
        "stale game present evidence");
  }
  if (game_presented_) {
    return DuplicateLocked("real game frame was already presented");
  }
  if (defer_until_native_success) {
    if (pending_present_) {
      return DuplicateLocked("game present evidence is already pending");
    }
    pending_present_ = true;
    pending_present_evidence_ = evidence;
    ++revision_;
    return {GameSessionUpdateStatus::kApplied, state_, Status::Ok(),
            "game present evidence pending native transition success"};
  }

  game_presented_ = true;
  first_presented_frame_ = evidence.frame_serial;
  ++revision_;
  return {GameSessionUpdateStatus::kApplied, state_, Status::Ok(),
          state_ == GameSessionState::kPaused
              ? "late last-surface game frame observed while paused"
              : "first real game frame presented"};
}

GameSessionUpdateResult GameSessionCoordinator::LeaveGame() {
  std::lock_guard<std::mutex> operation_lock(operation_mutex_);
  std::unique_lock<std::mutex> lock(mutex_);
  if (returned_to_lua_app_) {
    return DuplicateLocked("game already returned to LuaApp");
  }
  if (IsForwardTerminalLocked()) {
    return RejectTerminalLocked("leave game");
  }
  if (!initial_start_completed_ || (state_ != GameSessionState::kRunning &&
                                    state_ != GameSessionState::kPaused)) {
    return FailLocked(FailedPrecondition("LeaveGame requires a started game"),
                      "game leave arrived before initial start");
  }

  state_ = GameSessionState::kLeaving;
  ++revision_;
  lock.unlock();
  const Status status = capabilities_.leave_game(capabilities_.context);
  lock.lock();
  if (!status.ok()) {
    return FailLocked(status, "LeaveGame callback failed");
  }
  returned_to_lua_app_ = true;
  active_surface_available_ = false;
  surface_created_ = false;
  prepared_surface_generation_ = 0;
  game_paused_ = false;
  DiscardPendingSurfaceLocked();
  DiscardPendingPresentLocked();
  return ApplyLocked(GameSessionState::kReturnedToLuaApp,
                     "game returned to LuaApp; app runtime remains alive");
}

GameSessionUpdateResult GameSessionCoordinator::Shutdown() {
  std::lock_guard<std::mutex> operation_lock(operation_mutex_);
  std::unique_lock<std::mutex> lock(mutex_);
  if (state_ == GameSessionState::kStopped) {
    return DuplicateLocked("game runtime is already stopped");
  }
  if (shutdown_attempted_) {
    return RejectLocked(
        FailedPrecondition(
            "a failed shutdown is not retried after possible side effects"),
        "game runtime shutdown was already attempted");
  }
  if (capabilities_.shutdown_runtime == nullptr) {
    return FailLocked(
        Status::Error(StatusCode::kUnavailable,
                      "ShutdownGameRuntime capability is missing"),
        "cannot stop game runtime");
  }

  shutdown_attempted_ = true;
  state_ = GameSessionState::kStopping;
  ++revision_;
  lock.unlock();
  const Status status = capabilities_.shutdown_runtime(capabilities_.context);
  lock.lock();
  if (!status.ok()) {
    return FailLocked(status, "ShutdownGameRuntime callback failed");
  }
  active_surface_available_ = false;
  surface_created_ = false;
  prepared_surface_generation_ = 0;
  game_paused_ = false;
  DiscardPendingSurfaceLocked();
  DiscardPendingPresentLocked();
  return ApplyLocked(GameSessionState::kStopped,
                     "entire game app runtime stopped");
}

GameSessionSnapshot GameSessionCoordinator::Snapshot() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return SnapshotLocked();
}

GameSessionUpdateResult GameSessionCoordinator::StartIfReadyLocked(
    std::unique_lock<std::mutex>* lock, const char* accepted_message) {
  if (lock == nullptr || !lock->owns_lock() || !principal_ready_ ||
      !join_request_queued_ || !surface_created_ ||
      !pending_surface_available_ || initial_start_completed_) {
    return FailLocked(
        FailedPrecondition("initial StartGame prerequisites are incomplete"),
        "internal start precondition rejected");
  }

  state_ = GameSessionState::kStarting;
  ++revision_;
  DiscardPendingPresentLocked();
  const GameSessionPrincipal principal = principal_;
  const GameJoinRequest request = request_;
  const GameSurface surface = pending_surface_;
  lock->unlock();
  const Status status = capabilities_.start_game(capabilities_.context,
                                                 principal, request, surface);
  lock->lock();
  if (!status.ok()) {
    DiscardPendingSurfaceLocked();
    DiscardPendingPresentLocked();
    return FailLocked(status, "StartGame callback failed");
  }
  PromotePendingSurfaceLocked();
  initial_start_completed_ = true;
  game_paused_ = false;
  CommitPendingPresentLocked();
  return ApplyLocked(GameSessionState::kRunning,
                     std::string(accepted_message) +
                         "; initial game started without ResumeGame");
}

GameSessionUpdateResult GameSessionCoordinator::ApplyLocked(
    GameSessionState state, std::string message) {
  state_ = state;
  ++revision_;
  return {GameSessionUpdateStatus::kApplied, state_, Status::Ok(),
          std::move(message)};
}

GameSessionUpdateResult GameSessionCoordinator::DuplicateLocked(
    std::string message) const {
  return {GameSessionUpdateStatus::kDuplicate, state_, Status::Ok(),
          std::move(message)};
}

GameSessionUpdateResult GameSessionCoordinator::FailLocked(
    Status cause, std::string message) {
  failure_reason_ = message;
  if (!cause.message().empty()) {
    failure_reason_ += ": " + cause.message();
  }
  state_ = GameSessionState::kFailure;
  ++revision_;
  return {GameSessionUpdateStatus::kRejected, state_, std::move(cause),
          failure_reason_};
}

GameSessionUpdateResult GameSessionCoordinator::RejectLocked(
    Status cause, std::string message) const {
  return {GameSessionUpdateStatus::kRejected, state_, std::move(cause),
          std::move(message)};
}

GameSessionUpdateResult GameSessionCoordinator::RejectTerminalLocked(
    std::string operation) const {
  std::string message = std::move(operation) + " rejected in state " +
                        GameSessionStateName(state_);
  return RejectLocked(FailedPrecondition(message), message);
}

GameSessionSnapshot GameSessionCoordinator::SnapshotLocked() const {
  GameSessionSnapshot snapshot;
  snapshot.state = state_;
  snapshot.revision = revision_;
  snapshot.principal_ready = principal_ready_;
  snapshot.principal_kind = principal_.kind;
  snapshot.join_request_queued = join_request_queued_;
  snapshot.surface_created = surface_created_;
  snapshot.pending_surface_available = pending_surface_available_;
  snapshot.active_surface_available = active_surface_available_;
  snapshot.initial_start_completed = initial_start_completed_;
  snapshot.game_running = state_ == GameSessionState::kRunning ||
                          state_ == GameSessionState::kUpdatingSurface ||
                          state_ == GameSessionState::kPausing;
  snapshot.game_paused = game_paused_;
  snapshot.game_present_pending = pending_present_;
  snapshot.game_presented = game_presented_;
  snapshot.returned_to_lua_app = returned_to_lua_app_;
  snapshot.stopped = state_ == GameSessionState::kStopped;
  snapshot.principal_generation = principal_.generation;
  snapshot.request_id = request_.request_id;
  snapshot.prepared_surface_generation = prepared_surface_generation_;
  snapshot.pending_surface_generation = pending_surface_.generation;
  snapshot.active_surface_generation = active_surface_.generation;
  snapshot.active_surface_width = active_surface_.width;
  snapshot.active_surface_height = active_surface_.height;
  snapshot.first_presented_frame = first_presented_frame_;
  snapshot.failure_reason = failure_reason_;
  return snapshot;
}

GameSessionState GameSessionCoordinator::PreStartStateLocked() const {
  if (pending_surface_available_) {
    return GameSessionState::kSurfaceAvailable;
  }
  if (join_request_queued_) {
    return GameSessionState::kJoinRequestQueued;
  }
  if (surface_created_) {
    return GameSessionState::kSurfaceCreated;
  }
  return GameSessionState::kPrincipalReady;
}

bool GameSessionCoordinator::IsForwardTerminalLocked() const {
  return state_ == GameSessionState::kFailure ||
         state_ == GameSessionState::kReturnedToLuaApp ||
         state_ == GameSessionState::kStopping ||
         state_ == GameSessionState::kStopped;
}

bool GameSessionCoordinator::IsPresentShapeValidLocked(
    const GamePresentEvidence& evidence) const {
  return evidence.present_succeeded && evidence.frame_serial != 0 &&
         evidence.surface_generation != 0 &&
         evidence.backend != GamePresentBackend::kUnknown &&
         (requirements_.required_present_backend ==
              GamePresentBackend::kUnknown ||
          evidence.backend == requirements_.required_present_backend);
}

void GameSessionCoordinator::PromotePendingSurfaceLocked() {
  active_surface_ = pending_surface_;
  active_surface_available_ = true;
  DiscardPendingSurfaceLocked();
}

void GameSessionCoordinator::DiscardPendingSurfaceLocked() {
  pending_surface_available_ = false;
  pending_surface_ = {};
}

void GameSessionCoordinator::CommitPendingPresentLocked() {
  if (pending_present_ && !game_presented_) {
    game_presented_ = true;
    first_presented_frame_ = pending_present_evidence_.frame_serial;
    ++revision_;
  }
  DiscardPendingPresentLocked();
}

void GameSessionCoordinator::DiscardPendingPresentLocked() {
  pending_present_ = false;
  pending_present_evidence_ = {};
}

bool GameSessionCoordinator::SamePrincipal(const GameSessionPrincipal& left,
                                           const GameSessionPrincipal& right) {
  return left.kind == right.kind && left.generation == right.generation &&
         left.principal_id == right.principal_id &&
         left.base_url == right.base_url;
}

bool GameSessionCoordinator::SameRequest(const GameJoinRequest& left,
                                         const GameJoinRequest& right) {
  return left.request_id == right.request_id &&
         left.place_id == right.place_id &&
         left.launch_parameters == right.launch_parameters;
}

bool GameSessionCoordinator::SameSurface(const GameSurface& left,
                                         const GameSurface& right) {
  return left.generation == right.generation &&
         left.native_window == right.native_window &&
         left.width == right.width && left.height == right.height;
}

const char* GameSessionStateName(GameSessionState state) {
  switch (state) {
    case GameSessionState::kCreated:
      return "Created";
    case GameSessionState::kPrincipalReady:
      return "PrincipalReady";
    case GameSessionState::kJoinRequestQueued:
      return "JoinRequestQueued";
    case GameSessionState::kPreparingSurface:
      return "PreparingSurface";
    case GameSessionState::kSurfaceCreated:
      return "SurfaceCreated";
    case GameSessionState::kSurfaceAvailable:
      return "SurfaceAvailable";
    case GameSessionState::kStarting:
      return "Starting";
    case GameSessionState::kRunning:
      return "Running";
    case GameSessionState::kUpdatingSurface:
      return "UpdatingSurface";
    case GameSessionState::kPausing:
      return "Pausing";
    case GameSessionState::kPaused:
      return "Paused";
    case GameSessionState::kResuming:
      return "Resuming";
    case GameSessionState::kLeaving:
      return "Leaving";
    case GameSessionState::kReturnedToLuaApp:
      return "ReturnedToLuaApp";
    case GameSessionState::kFailure:
      return "Failure";
    case GameSessionState::kStopping:
      return "Stopping";
    case GameSessionState::kStopped:
      return "Stopped";
  }
  return "Unknown";
}

const char* GamePresentBackendName(GamePresentBackend backend) {
  switch (backend) {
    case GamePresentBackend::kUnknown:
      return "Unknown";
    case GamePresentBackend::kVulkan:
      return "Vulkan";
    case GamePresentBackend::kOpenGlEs:
      return "OpenGlEs";
  }
  return "Unknown";
}

const char* GameSessionPrincipalKindName(GameSessionPrincipalKind kind) {
  switch (kind) {
    case GameSessionPrincipalKind::kLocalGuest:
      return "LocalGuest";
    case GameSessionPrincipalKind::kAuthenticated:
      return "Authenticated";
  }
  return "Unknown";
}

}  // namespace runtime
}  // namespace mocktail
