#ifndef MOCKTAIL_RUNTIME_GAME_SESSION_COORDINATOR_H_
#define MOCKTAIL_RUNTIME_GAME_SESSION_COORDINATOR_H_

#include <cstdint>
#include <mutex>
#include <string>

#include "mocktail/status.h"

namespace mocktail {
namespace runtime {

enum class GameSessionPrincipalKind {
  kLocalGuest,
  kAuthenticated,
};

// Session identity carries no cookie or bearer token. The owning
// auth service keeps secrets and exposes only a monotonically increasing
// generation to the runtime coordinator. A local guest has no principal ID and
// is valid only for the local placeId=0 graphics path.
struct GameSessionPrincipal {
  GameSessionPrincipalKind kind = GameSessionPrincipalKind::kLocalGuest;
  uint64_t generation = 0;
  std::string principal_id;
  std::string base_url;
};

// Opaque launch data remains owned by the join service. This keeps the state
// machine stable while Roblox changes the exact StartGameParams schema.
struct GameJoinRequest {
  uint64_t request_id = 0;
  int64_t place_id = 0;
  std::string launch_parameters;
};

// A generation identifies one native surface lifetime. Recreated Android
// surfaces must receive a new generation even when the host window is reused.
struct GameSurface {
  uint64_t generation = 0;
  uintptr_t native_window = 0;
  uint32_t width = 0;
  uint32_t height = 0;
};

enum class GamePresentBackend {
  kUnknown,
  kVulkan,
  kOpenGlEs,
};

// This evidence is expected to be emitted only after the graphics backend has
// observed a successful native present operation.
struct GamePresentEvidence {
  uint64_t surface_generation = 0;
  uint64_t frame_serial = 0;
  GamePresentBackend backend = GamePresentBackend::kUnknown;
  bool present_succeeded = false;
};

// Maps ExperienceSession.onSurfaceCreated to
// setTaskSchedulerBackgroundMode(false, "ES.onSurfaceCreated"). The adapter,
// rather than this policy layer, owns those JNI-specific arguments.
using SetGameForegroundFn = Status (*)(void* context);
using StartGameFn = Status (*)(void* context,
                               const GameSessionPrincipal& principal,
                               const GameJoinRequest& request,
                               const GameSurface& surface);
using UpdateGameSurfaceFn = Status (*)(void* context,
                                       const GameSurface& surface);
using PauseGameFn = Status (*)(void* context);
using ResumeGameFn = Status (*)(void* context, const GameSurface& surface);
using LeaveGameFn = Status (*)(void* context);
using ShutdownGameRuntimeFn = Status (*)(void* context);

// These callbacks form the native boundary. They are synchronous and
// non-throwing; the coordinator serializes lifecycle calls to match
// MainGameActivity's single-thread executor.
struct GameSessionCapabilities {
  void* context = nullptr;
  SetGameForegroundFn set_game_foreground = nullptr;
  StartGameFn start_game = nullptr;
  UpdateGameSurfaceFn update_surface = nullptr;
  PauseGameFn pause_game = nullptr;
  ResumeGameFn resume_game = nullptr;
  LeaveGameFn leave_game = nullptr;
  ShutdownGameRuntimeFn shutdown_runtime = nullptr;

  bool complete() const;
};

struct GameSessionRequirements {
  GamePresentBackend required_present_backend = GamePresentBackend::kVulkan;
};

enum class GameSessionState {
  kCreated,
  kPrincipalReady,
  kJoinRequestQueued,
  kPreparingSurface,
  kSurfaceCreated,
  kSurfaceAvailable,
  kStarting,
  kRunning,
  kUpdatingSurface,
  kPausing,
  kPaused,
  kResuming,
  kLeaving,
  kReturnedToLuaApp,
  kFailure,
  kStopping,
  kStopped,
};

enum class GameSessionUpdateStatus {
  kApplied,
  kDuplicate,
  kRejected,
};

struct GameSessionUpdateResult {
  GameSessionUpdateStatus status = GameSessionUpdateStatus::kRejected;
  GameSessionState state = GameSessionState::kCreated;
  Status cause = Status::Ok();
  std::string message;

  bool ok() const { return status != GameSessionUpdateStatus::kRejected; }
  bool changed() const { return status == GameSessionUpdateStatus::kApplied; }
};

struct GameSessionSnapshot {
  GameSessionState state = GameSessionState::kCreated;
  uint64_t revision = 0;
  bool principal_ready = false;
  GameSessionPrincipalKind principal_kind =
      GameSessionPrincipalKind::kLocalGuest;
  bool join_request_queued = false;
  bool surface_created = false;
  bool pending_surface_available = false;
  bool active_surface_available = false;
  bool initial_start_completed = false;
  bool game_running = false;
  bool game_paused = false;
  bool game_present_pending = false;
  bool game_presented = false;
  bool returned_to_lua_app = false;
  bool stopped = false;
  uint64_t principal_generation = 0;
  uint64_t request_id = 0;
  uint64_t prepared_surface_generation = 0;
  uint64_t pending_surface_generation = 0;
  uint64_t active_surface_generation = 0;
  uint32_t active_surface_width = 0;
  uint32_t active_surface_height = 0;
  uint64_t first_presented_frame = 0;
  std::string failure_reason;
};

// SurfaceCreated must open a generation before its matching SurfaceChanged can
// supply a window and size. A join request and the first surface may arrive in
// either order; initial startup calls StartGame, never ResumeGame. Once running,
// new generations pause and resume the session. A candidate surface becomes
// active only after its native callback succeeds.
class GameSessionCoordinator final {
 public:
  explicit GameSessionCoordinator(GameSessionCapabilities capabilities,
                                  GameSessionRequirements requirements = {});

  GameSessionCoordinator(const GameSessionCoordinator&) = delete;
  GameSessionCoordinator& operator=(const GameSessionCoordinator&) = delete;
  GameSessionCoordinator(GameSessionCoordinator&&) = delete;
  GameSessionCoordinator& operator=(GameSessionCoordinator&&) = delete;

  GameSessionUpdateResult SetPrincipal(GameSessionPrincipal principal);
  GameSessionUpdateResult QueueJoinRequest(GameJoinRequest request);
  GameSessionUpdateResult SurfaceCreated(uint64_t surface_generation);
  GameSessionUpdateResult SurfaceChanged(GameSurface surface);
  // Replays the native surface update even when the handle and extent are
  // unchanged. Fullscreen compositors can invalidate a Vulkan swapchain after
  // their final state transition without changing the last reported extent.
  GameSessionUpdateResult RebindSurface(GameSurface surface);
  GameSessionUpdateResult SurfaceDestroyed(uint64_t surface_generation);
  GameSessionUpdateResult RecordGamePresent(GamePresentEvidence evidence);

  // Leaves the current experience and returns to LuaApp. It keeps
  // the app runtime alive so composition may start another session later.
  GameSessionUpdateResult LeaveGame();

  // Terminates the entire app runtime. It is not an alias for LeaveGame and
  // never injects an implicit leave callback before app-level teardown.
  GameSessionUpdateResult Shutdown();

  GameSessionSnapshot Snapshot() const;

 private:
  GameSessionUpdateResult StartIfReadyLocked(std::unique_lock<std::mutex>* lock,
                                             const char* accepted_message);
  GameSessionUpdateResult UpdateSurface(GameSurface surface, bool force_rebind);
  GameSessionUpdateResult ApplyLocked(GameSessionState state,
                                      std::string message);
  GameSessionUpdateResult DuplicateLocked(std::string message) const;
  GameSessionUpdateResult FailLocked(Status cause, std::string message);
  GameSessionUpdateResult RejectLocked(Status cause, std::string message) const;
  GameSessionUpdateResult RejectTerminalLocked(std::string operation) const;
  GameSessionSnapshot SnapshotLocked() const;
  GameSessionState PreStartStateLocked() const;
  bool IsForwardTerminalLocked() const;
  bool IsPresentShapeValidLocked(const GamePresentEvidence& evidence) const;
  void PromotePendingSurfaceLocked();
  void DiscardPendingSurfaceLocked();
  void CommitPendingPresentLocked();
  void DiscardPendingPresentLocked();

  static bool SamePrincipal(const GameSessionPrincipal& left,
                            const GameSessionPrincipal& right);
  static bool SameRequest(const GameJoinRequest& left,
                          const GameJoinRequest& right);
  static bool SameSurface(const GameSurface& left, const GameSurface& right);

  // Lifecycle methods take operation_mutex_ before mutex_. Native callbacks
  // run without mutex_, so Vulkan evidence can be recorded synchronously. A
  // callback must not re-enter another lifecycle method; render evidence is the
  // only supported synchronous re-entry.
  mutable std::mutex operation_mutex_;
  mutable std::mutex mutex_;
  const GameSessionCapabilities capabilities_;
  const GameSessionRequirements requirements_;

  GameSessionState state_ = GameSessionState::kCreated;
  uint64_t revision_ = 0;
  bool principal_ready_ = false;
  bool join_request_queued_ = false;
  bool surface_created_ = false;
  bool pending_surface_available_ = false;
  bool active_surface_available_ = false;
  bool initial_start_completed_ = false;
  bool game_paused_ = false;
  bool pending_present_ = false;
  bool game_presented_ = false;
  bool returned_to_lua_app_ = false;
  bool shutdown_attempted_ = false;
  uint64_t prepared_surface_generation_ = 0;
  uint64_t last_destroyed_surface_generation_ = 0;
  GameSessionPrincipal principal_;
  GameJoinRequest request_;
  GameSurface pending_surface_;
  GameSurface active_surface_;
  GamePresentEvidence pending_present_evidence_;
  uint64_t first_presented_frame_ = 0;
  std::string failure_reason_;
};

const char* GameSessionStateName(GameSessionState state);
const char* GamePresentBackendName(GamePresentBackend backend);
const char* GameSessionPrincipalKindName(GameSessionPrincipalKind kind);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_GAME_SESSION_COORDINATOR_H_
