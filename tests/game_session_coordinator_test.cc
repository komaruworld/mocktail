#include "runtime/game_session_coordinator.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace mocktail {
namespace runtime {
namespace {

struct Probe {
  Status Invoke(const char* call) {
    std::unique_lock<std::mutex> lock(mutex);
    calls.emplace_back(call);
    ++active_callbacks;
    max_active_callbacks = std::max(max_active_callbacks, active_callbacks);
    if (blocked_call == call) {
      blocked_call_entered = true;
      condition.notify_all();
      if (!condition.wait_for(lock, std::chrono::seconds(2),
                              [this] { return release_blocked_call; })) {
        --active_callbacks;
        return Status::Error(StatusCode::kUnavailable,
                             std::string(call) + " timed out in test probe");
      }
    }
    --active_callbacks;
    if (failed_call == call) {
      return Status::Error(StatusCode::kPlatformError,
                           std::string(call) + " injected failure");
    }
    return Status::Ok();
  }

  std::mutex mutex;
  std::condition_variable condition;
  std::vector<std::string> calls;
  std::string failed_call;
  std::string blocked_call;
  bool blocked_call_entered = false;
  bool release_blocked_call = false;
  int active_callbacks = 0;
  int max_active_callbacks = 0;
  GameSessionPrincipal start_principal;
  GameJoinRequest start_request;
  GameSurface start_surface;
  GameSurface updated_surface;
  GameSurface resumed_surface;
};

Status Foreground(void* context) {
  return static_cast<Probe*>(context)->Invoke("foreground");
}

Status Start(void* context, const GameSessionPrincipal& principal,
             const GameJoinRequest& request, const GameSurface& surface) {
  auto* probe = static_cast<Probe*>(context);
  probe->start_principal = principal;
  probe->start_request = request;
  probe->start_surface = surface;
  return probe->Invoke("start");
}

Status Update(void* context, const GameSurface& surface) {
  auto* probe = static_cast<Probe*>(context);
  probe->updated_surface = surface;
  return probe->Invoke("update");
}

Status Pause(void* context) {
  return static_cast<Probe*>(context)->Invoke("pause");
}

Status Resume(void* context, const GameSurface& surface) {
  auto* probe = static_cast<Probe*>(context);
  probe->resumed_surface = surface;
  return probe->Invoke("resume");
}

Status Leave(void* context) {
  return static_cast<Probe*>(context)->Invoke("leave");
}

Status Shutdown(void* context) {
  return static_cast<Probe*>(context)->Invoke("shutdown");
}

GameSessionCapabilities Capabilities(Probe* probe) {
  return {probe, Foreground, Start, Update, Pause, Resume, Leave, Shutdown};
}

GameSessionPrincipal AuthenticatedPrincipal(uint64_t generation = 11) {
  return {GameSessionPrincipalKind::kAuthenticated, generation, "123456",
          "https://www.roblox.com"};
}

GameSessionPrincipal LocalGuestPrincipal(uint64_t generation = 11) {
  return {GameSessionPrincipalKind::kLocalGuest,
          generation,
          {},
          "https://www.roblox.com"};
}

GameJoinRequest JoinRequest(uint64_t request_id = 22,
                            std::string parameters = "join-payload",
                            int64_t place_id = 123) {
  return {request_id, place_id, std::move(parameters)};
}

GameSurface Surface(uint64_t generation = 33, uintptr_t native_window = 0x44,
                    uint32_t width = 1280, uint32_t height = 720) {
  return {generation, native_window, width, height};
}

GamePresentEvidence Present(uint64_t surface_generation = 33,
                            uint64_t frame_serial = 1) {
  return {surface_generation, frame_serial, GamePresentBackend::kVulkan, true};
}

void SetAuthenticatedPrincipalOrFail(GameSessionCoordinator* coordinator) {
  ASSERT_NE(coordinator, nullptr);
  ASSERT_TRUE(coordinator->SetPrincipal(AuthenticatedPrincipal()).ok());
}

void StartJoinFirstOrFail(GameSessionCoordinator* coordinator) {
  SetAuthenticatedPrincipalOrFail(coordinator);
  ASSERT_TRUE(coordinator->QueueJoinRequest(JoinRequest()).ok());
  ASSERT_TRUE(coordinator->SurfaceCreated(33).ok());
  ASSERT_TRUE(coordinator->SurfaceChanged(Surface()).ok());
  ASSERT_EQ(coordinator->Snapshot().state, GameSessionState::kRunning);
}

bool WaitForBlockedCall(Probe* probe) {
  std::unique_lock<std::mutex> lock(probe->mutex);
  return probe->condition.wait_for(lock, std::chrono::seconds(2), [probe] {
    return probe->blocked_call_entered;
  });
}

void ReleaseBlockedCall(Probe* probe) {
  {
    std::lock_guard<std::mutex> lock(probe->mutex);
    probe->release_blocked_call = true;
  }
  probe->condition.notify_all();
}

TEST(GameSessionCoordinatorTest, JoinThenSurfaceStartsWithCurrentSurface) {
  Probe probe;
  GameSessionCoordinator coordinator(Capabilities(&probe));

  ASSERT_TRUE(coordinator.SetPrincipal(AuthenticatedPrincipal()).ok());
  EXPECT_EQ(coordinator.QueueJoinRequest(JoinRequest()).state,
            GameSessionState::kJoinRequestQueued);
  EXPECT_TRUE(probe.calls.empty());

  ASSERT_TRUE(coordinator.SurfaceCreated(33).ok());

  const GameSessionUpdateResult result = coordinator.SurfaceChanged(Surface());

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.state, GameSessionState::kRunning);
  EXPECT_EQ(probe.calls, std::vector<std::string>({"foreground", "start"}));
  EXPECT_EQ(probe.start_principal.generation, 11U);
  EXPECT_EQ(probe.start_request.request_id, 22U);
  EXPECT_EQ(probe.start_surface.generation, 33U);
  EXPECT_TRUE(coordinator.Snapshot().initial_start_completed);
  EXPECT_FALSE(coordinator.Snapshot().game_paused);
}

TEST(GameSessionCoordinatorTest, SurfaceThenJoinStartsWithoutInitialResume) {
  Probe probe;
  GameSessionCoordinator coordinator(Capabilities(&probe));

  ASSERT_TRUE(coordinator.SetPrincipal(AuthenticatedPrincipal()).ok());
  ASSERT_TRUE(coordinator.SurfaceCreated(33).ok());
  EXPECT_EQ(coordinator.SurfaceChanged(Surface()).state,
            GameSessionState::kSurfaceAvailable);
  EXPECT_EQ(probe.calls, std::vector<std::string>({"foreground"}));

  const GameSessionUpdateResult result =
      coordinator.QueueJoinRequest(JoinRequest());

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.state, GameSessionState::kRunning);
  EXPECT_EQ(probe.calls, std::vector<std::string>({"foreground", "start"}));
}

TEST(GameSessionCoordinatorTest, SurfaceChangedRequiresPreparedGeneration) {
  Probe probe;
  GameSessionCoordinator coordinator(Capabilities(&probe));
  SetAuthenticatedPrincipalOrFail(&coordinator);

  const GameSessionUpdateResult result = coordinator.SurfaceChanged(Surface());

  EXPECT_EQ(result.status, GameSessionUpdateStatus::kRejected);
  EXPECT_EQ(result.state, GameSessionState::kFailure);
  EXPECT_EQ(result.cause.code(), StatusCode::kFailedPrecondition);
  EXPECT_TRUE(probe.calls.empty());
}

TEST(GameSessionCoordinatorTest, SurfaceCreatedForegroundFailureFailsClosed) {
  Probe probe;
  probe.failed_call = "foreground";
  GameSessionCoordinator coordinator(Capabilities(&probe));
  SetAuthenticatedPrincipalOrFail(&coordinator);

  const GameSessionUpdateResult result = coordinator.SurfaceCreated(33);

  EXPECT_EQ(result.state, GameSessionState::kFailure);
  EXPECT_EQ(result.cause.code(), StatusCode::kPlatformError);
  EXPECT_FALSE(coordinator.Snapshot().surface_created);
  EXPECT_EQ(probe.calls, std::vector<std::string>({"foreground"}));
}

TEST(GameSessionCoordinatorTest, LatestPreStartSurfaceIsPassedToStart) {
  Probe probe;
  GameSessionCoordinator coordinator(Capabilities(&probe));
  SetAuthenticatedPrincipalOrFail(&coordinator);
  ASSERT_TRUE(coordinator.SurfaceCreated(33).ok());
  ASSERT_TRUE(coordinator.SurfaceChanged(Surface()).ok());
  ASSERT_TRUE(coordinator.SurfaceChanged(Surface(33, 0x45, 1920, 1080)).ok());

  ASSERT_TRUE(coordinator.QueueJoinRequest(JoinRequest()).ok());

  EXPECT_EQ(probe.calls, std::vector<std::string>({"foreground", "start"}));
  EXPECT_EQ(probe.start_surface.generation, 33U);
  EXPECT_EQ(probe.start_surface.width, 1920U);
}

TEST(GameSessionCoordinatorTest, RunningSurfaceChangeCallsUpdateOnly) {
  Probe probe;
  GameSessionCoordinator coordinator(Capabilities(&probe));
  StartJoinFirstOrFail(&coordinator);

  const GameSessionUpdateResult result =
      coordinator.SurfaceChanged(Surface(33, 0x55, 1600, 900));

  EXPECT_TRUE(result.ok());
  EXPECT_EQ(result.state, GameSessionState::kRunning);
  EXPECT_EQ(probe.calls,
            std::vector<std::string>({"foreground", "start", "update"}));
  EXPECT_EQ(probe.updated_surface.generation, 33U);
}

TEST(GameSessionCoordinatorTest,
     AuthoritativeRebindUpdatesAnUnchangedRunningSurface) {
  Probe probe;
  GameSessionCoordinator coordinator(Capabilities(&probe));
  StartJoinFirstOrFail(&coordinator);

  const GameSessionUpdateResult result = coordinator.RebindSurface(Surface());

  EXPECT_TRUE(result.ok());
  EXPECT_EQ(result.status, GameSessionUpdateStatus::kApplied);
  EXPECT_EQ(result.state, GameSessionState::kRunning);
  EXPECT_EQ(probe.calls,
            std::vector<std::string>({"foreground", "start", "update"}));
  EXPECT_EQ(probe.updated_surface.generation, 33U);
  EXPECT_EQ(probe.updated_surface.width, 1280U);
  EXPECT_EQ(probe.updated_surface.height, 720U);
}

TEST(GameSessionCoordinatorTest, DestroyAndRecreatePausesThenResumes) {
  Probe probe;
  GameSessionCoordinator coordinator(Capabilities(&probe));
  StartJoinFirstOrFail(&coordinator);

  EXPECT_EQ(coordinator.SurfaceDestroyed(33).state, GameSessionState::kPaused);
  EXPECT_FALSE(coordinator.Snapshot().active_surface_available);
  EXPECT_TRUE(coordinator.Snapshot().game_paused);

  ASSERT_TRUE(coordinator.SurfaceCreated(34).ok());

  const GameSessionUpdateResult resumed =
      coordinator.SurfaceChanged(Surface(34, 0x55));

  EXPECT_EQ(resumed.state, GameSessionState::kRunning);
  EXPECT_EQ(probe.calls,
            std::vector<std::string>(
                {"foreground", "start", "pause", "foreground", "resume"}));
  EXPECT_EQ(probe.resumed_surface.generation, 34U);
  EXPECT_FALSE(coordinator.Snapshot().game_paused);
}

TEST(GameSessionCoordinatorTest, DestroyBeforeStartDropsStaleSurface) {
  Probe probe;
  GameSessionCoordinator coordinator(Capabilities(&probe));
  SetAuthenticatedPrincipalOrFail(&coordinator);
  ASSERT_TRUE(coordinator.SurfaceCreated(33).ok());
  ASSERT_TRUE(coordinator.SurfaceChanged(Surface()).ok());

  EXPECT_EQ(coordinator.SurfaceDestroyed(33).state,
            GameSessionState::kPrincipalReady);
  EXPECT_EQ(coordinator.SurfaceDestroyed(33).status,
            GameSessionUpdateStatus::kDuplicate);
  EXPECT_EQ(probe.calls, std::vector<std::string>({"foreground"}));
  ASSERT_TRUE(coordinator.QueueJoinRequest(JoinRequest()).ok());
  EXPECT_EQ(coordinator.Snapshot().state, GameSessionState::kJoinRequestQueued);
  EXPECT_EQ(probe.calls, std::vector<std::string>({"foreground"}));
}

TEST(GameSessionCoordinatorTest, LatePresentedFrameAfterPauseIsIdempotent) {
  Probe probe;
  GameSessionCoordinator coordinator(Capabilities(&probe));
  StartJoinFirstOrFail(&coordinator);
  ASSERT_TRUE(coordinator.RecordGamePresent(Present()).ok());
  ASSERT_TRUE(coordinator.SurfaceDestroyed(33).ok());

  const GameSessionUpdateResult late =
      coordinator.RecordGamePresent(Present(33, 2));

  EXPECT_EQ(late.status, GameSessionUpdateStatus::kDuplicate);
  EXPECT_EQ(late.state, GameSessionState::kPaused);
  EXPECT_EQ(coordinator.Snapshot().state, GameSessionState::kPaused);
}

TEST(GameSessionCoordinatorTest, FirstLateFrameAfterPauseIsAccepted) {
  Probe probe;
  GameSessionCoordinator coordinator(Capabilities(&probe));
  StartJoinFirstOrFail(&coordinator);
  ASSERT_TRUE(coordinator.SurfaceDestroyed(33).ok());

  const GameSessionUpdateResult late =
      coordinator.RecordGamePresent(Present(33, 7));

  EXPECT_EQ(late.status, GameSessionUpdateStatus::kApplied);
  EXPECT_EQ(late.state, GameSessionState::kPaused);
  EXPECT_TRUE(coordinator.Snapshot().game_presented);
  EXPECT_EQ(coordinator.Snapshot().first_presented_frame, 7U);
}

TEST(GameSessionCoordinatorTest, PresentDuringStartCommitsAfterNativeSuccess) {
  Probe probe;
  probe.blocked_call = "start";
  GameSessionCoordinator coordinator(Capabilities(&probe));
  SetAuthenticatedPrincipalOrFail(&coordinator);
  ASSERT_TRUE(coordinator.QueueJoinRequest(JoinRequest()).ok());
  ASSERT_TRUE(coordinator.SurfaceCreated(33).ok());

  GameSessionUpdateResult start_result;
  std::thread start_thread(
      [&] { start_result = coordinator.SurfaceChanged(Surface()); });
  const bool entered = WaitForBlockedCall(&probe);
  GameSessionUpdateResult present_result;
  if (entered) {
    present_result = coordinator.RecordGamePresent(Present(33, 9));
  }
  ReleaseBlockedCall(&probe);
  start_thread.join();

  ASSERT_TRUE(entered);
  EXPECT_EQ(present_result.status, GameSessionUpdateStatus::kApplied);
  EXPECT_EQ(present_result.state, GameSessionState::kStarting);
  EXPECT_TRUE(start_result.ok());
  EXPECT_EQ(start_result.state, GameSessionState::kRunning);
  EXPECT_FALSE(coordinator.Snapshot().game_present_pending);
  EXPECT_TRUE(coordinator.Snapshot().game_presented);
  EXPECT_EQ(coordinator.Snapshot().first_presented_frame, 9U);
}

TEST(GameSessionCoordinatorTest, RealVulkanPresentAndLeaveAreExplicit) {
  Probe probe;
  GameSessionCoordinator coordinator(Capabilities(&probe));
  StartJoinFirstOrFail(&coordinator);

  const GameSessionUpdateResult presented =
      coordinator.RecordGamePresent(Present());
  ASSERT_TRUE(presented.ok());
  EXPECT_EQ(presented.state, GameSessionState::kRunning);
  EXPECT_TRUE(coordinator.Snapshot().game_presented);
  EXPECT_EQ(coordinator.Snapshot().first_presented_frame, 1U);

  EXPECT_EQ(coordinator.LeaveGame().state, GameSessionState::kReturnedToLuaApp);
  EXPECT_TRUE(coordinator.Snapshot().returned_to_lua_app);
  EXPECT_EQ(probe.calls,
            std::vector<std::string>({"foreground", "start", "leave"}));
}

TEST(GameSessionCoordinatorTest, IdenticalEventsAreIdempotent) {
  Probe probe;
  GameSessionCoordinator coordinator(Capabilities(&probe));
  ASSERT_EQ(coordinator.SetPrincipal(AuthenticatedPrincipal()).status,
            GameSessionUpdateStatus::kApplied);
  EXPECT_EQ(coordinator.SetPrincipal(AuthenticatedPrincipal()).status,
            GameSessionUpdateStatus::kDuplicate);
  ASSERT_EQ(coordinator.QueueJoinRequest(JoinRequest()).status,
            GameSessionUpdateStatus::kApplied);
  EXPECT_EQ(coordinator.QueueJoinRequest(JoinRequest()).status,
            GameSessionUpdateStatus::kDuplicate);
  ASSERT_TRUE(coordinator.SurfaceCreated(33).ok());
  EXPECT_EQ(coordinator.SurfaceCreated(33).status,
            GameSessionUpdateStatus::kDuplicate);
  ASSERT_TRUE(coordinator.SurfaceChanged(Surface()).ok());
  EXPECT_EQ(coordinator.SurfaceChanged(Surface()).status,
            GameSessionUpdateStatus::kDuplicate);
  ASSERT_TRUE(coordinator.RecordGamePresent(Present()).ok());
  EXPECT_EQ(coordinator.RecordGamePresent(Present(33, 2)).status,
            GameSessionUpdateStatus::kDuplicate);
  ASSERT_TRUE(coordinator.SurfaceDestroyed(33).ok());
  EXPECT_EQ(coordinator.SurfaceDestroyed(33).status,
            GameSessionUpdateStatus::kDuplicate);
  ASSERT_TRUE(coordinator.SurfaceCreated(34).ok());
  ASSERT_TRUE(coordinator.SurfaceChanged(Surface(34, 0x55)).ok());
  ASSERT_TRUE(coordinator.LeaveGame().ok());
  EXPECT_EQ(coordinator.LeaveGame().status,
            GameSessionUpdateStatus::kDuplicate);
  ASSERT_TRUE(coordinator.Shutdown().ok());
  EXPECT_EQ(coordinator.Shutdown().status, GameSessionUpdateStatus::kDuplicate);

  EXPECT_EQ(probe.calls, std::vector<std::string>(
                             {"foreground", "start", "pause", "foreground",
                              "resume", "leave", "shutdown"}));
}

TEST(GameSessionCoordinatorTest, JoinBeforePrincipalFailsClosed) {
  Probe probe;
  GameSessionCoordinator coordinator(Capabilities(&probe));

  const GameSessionUpdateResult result =
      coordinator.QueueJoinRequest(JoinRequest());

  EXPECT_EQ(result.status, GameSessionUpdateStatus::kRejected);
  EXPECT_EQ(result.state, GameSessionState::kFailure);
  EXPECT_EQ(result.cause.code(), StatusCode::kFailedPrecondition);
  EXPECT_TRUE(probe.calls.empty());
  EXPECT_FALSE(coordinator.SetPrincipal(AuthenticatedPrincipal()).ok());
}

TEST(GameSessionCoordinatorTest, LocalGuestCanStartOnlyLocalPlaceZero) {
  Probe probe;
  GameSessionCoordinator coordinator(Capabilities(&probe));
  ASSERT_TRUE(coordinator.SetPrincipal(LocalGuestPrincipal()).ok());
  ASSERT_TRUE(
      coordinator.QueueJoinRequest(JoinRequest(22, "local-graphics-payload", 0))
          .ok());
  ASSERT_TRUE(coordinator.SurfaceCreated(33).ok());

  const GameSessionUpdateResult result = coordinator.SurfaceChanged(Surface());

  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.state, GameSessionState::kRunning);
  EXPECT_EQ(probe.start_principal.kind, GameSessionPrincipalKind::kLocalGuest);
  EXPECT_EQ(probe.start_request.place_id, 0);
  EXPECT_EQ(probe.calls, std::vector<std::string>({"foreground", "start"}));
  const GameSessionSnapshot snapshot = coordinator.Snapshot();
  EXPECT_TRUE(snapshot.principal_ready);
  EXPECT_EQ(snapshot.principal_kind, GameSessionPrincipalKind::kLocalGuest);
  EXPECT_EQ(snapshot.principal_generation, 11U);
}

TEST(GameSessionCoordinatorTest, LocalGuestNetworkJoinFailsClosed) {
  Probe probe;
  GameSessionCoordinator coordinator(Capabilities(&probe));
  ASSERT_TRUE(coordinator.SetPrincipal(LocalGuestPrincipal()).ok());

  const GameSessionUpdateResult result =
      coordinator.QueueJoinRequest(JoinRequest());

  EXPECT_EQ(result.status, GameSessionUpdateStatus::kRejected);
  EXPECT_EQ(result.state, GameSessionState::kFailure);
  EXPECT_EQ(result.cause.code(), StatusCode::kFailedPrecondition);
  EXPECT_TRUE(probe.calls.empty());
  EXPECT_FALSE(coordinator.Snapshot().join_request_queued);
}

TEST(GameSessionCoordinatorTest, LocalGuestCannotCarryAccountIdentity) {
  Probe probe;
  GameSessionCoordinator coordinator(Capabilities(&probe));
  GameSessionPrincipal guest = LocalGuestPrincipal();
  guest.principal_id = "123456";

  const GameSessionUpdateResult result = coordinator.SetPrincipal(guest);

  EXPECT_EQ(result.status, GameSessionUpdateStatus::kRejected);
  EXPECT_EQ(result.state, GameSessionState::kFailure);
  EXPECT_EQ(result.cause.code(), StatusCode::kInvalidArgument);
  EXPECT_FALSE(coordinator.Snapshot().principal_ready);
  EXPECT_TRUE(probe.calls.empty());
}

TEST(GameSessionCoordinatorTest, ConflictingDuplicateFailsClosed) {
  Probe probe;
  GameSessionCoordinator coordinator(Capabilities(&probe));
  SetAuthenticatedPrincipalOrFail(&coordinator);
  ASSERT_TRUE(coordinator.QueueJoinRequest(JoinRequest()).ok());

  const GameSessionUpdateResult result =
      coordinator.QueueJoinRequest(JoinRequest(23, "different"));

  EXPECT_EQ(result.state, GameSessionState::kFailure);
  EXPECT_EQ(result.cause.code(), StatusCode::kFailedPrecondition);
  EXPECT_TRUE(probe.calls.empty());
}

TEST(GameSessionCoordinatorTest, InvalidPresentEvidenceIsNonDestructive) {
  Probe probe;
  GameSessionCoordinator coordinator(Capabilities(&probe));
  StartJoinFirstOrFail(&coordinator);
  GamePresentEvidence evidence = Present();
  evidence.backend = GamePresentBackend::kOpenGlEs;

  const GameSessionUpdateResult result =
      coordinator.RecordGamePresent(evidence);

  EXPECT_EQ(result.state, GameSessionState::kRunning);
  EXPECT_EQ(result.cause.code(), StatusCode::kInvalidArgument);
  EXPECT_FALSE(coordinator.Snapshot().game_presented);
}

TEST(GameSessionCoordinatorTest, RecreatedSurfaceNeedsNewGeneration) {
  Probe probe;
  GameSessionCoordinator coordinator(Capabilities(&probe));
  StartJoinFirstOrFail(&coordinator);
  ASSERT_TRUE(coordinator.SurfaceDestroyed(33).ok());

  const GameSessionUpdateResult result = coordinator.SurfaceCreated(33);

  EXPECT_EQ(result.state, GameSessionState::kFailure);
  EXPECT_EQ(probe.calls,
            std::vector<std::string>({"foreground", "start", "pause"}));
}

TEST(GameSessionCoordinatorTest, StalePresentAfterResumeIsNonDestructive) {
  Probe probe;
  GameSessionCoordinator coordinator(Capabilities(&probe));
  StartJoinFirstOrFail(&coordinator);
  ASSERT_TRUE(coordinator.SurfaceDestroyed(33).ok());
  ASSERT_TRUE(coordinator.SurfaceCreated(34).ok());
  ASSERT_TRUE(coordinator.SurfaceChanged(Surface(34, 0x55)).ok());

  const GameSessionUpdateResult stale =
      coordinator.RecordGamePresent(Present(33, 4));

  EXPECT_EQ(stale.status, GameSessionUpdateStatus::kRejected);
  EXPECT_EQ(stale.state, GameSessionState::kRunning);
  EXPECT_FALSE(coordinator.Snapshot().game_presented);
  EXPECT_TRUE(coordinator.RecordGamePresent(Present(34, 5)).ok());
  EXPECT_EQ(coordinator.Snapshot().first_presented_frame, 5U);
}

TEST(GameSessionCoordinatorTest, IncompleteCapabilitiesFailBeforeNativeWork) {
  Probe probe;
  GameSessionCapabilities capabilities = Capabilities(&probe);
  capabilities.resume_game = nullptr;
  GameSessionCoordinator coordinator(capabilities);

  const GameSessionUpdateResult result =
      coordinator.SetPrincipal(AuthenticatedPrincipal());

  EXPECT_EQ(result.state, GameSessionState::kFailure);
  EXPECT_EQ(result.cause.code(), StatusCode::kUnavailable);
  EXPECT_TRUE(probe.calls.empty());
}

TEST(GameSessionCoordinatorTest, StartFailureIsPreservedAndCanShutdown) {
  Probe probe;
  probe.failed_call = "start";
  GameSessionCoordinator coordinator(Capabilities(&probe));
  SetAuthenticatedPrincipalOrFail(&coordinator);
  ASSERT_TRUE(coordinator.QueueJoinRequest(JoinRequest()).ok());
  ASSERT_TRUE(coordinator.SurfaceCreated(33).ok());

  const GameSessionUpdateResult start = coordinator.SurfaceChanged(Surface());

  EXPECT_EQ(start.state, GameSessionState::kFailure);
  EXPECT_EQ(start.cause.code(), StatusCode::kPlatformError);
  EXPECT_FALSE(coordinator.Snapshot().initial_start_completed);
  probe.failed_call.clear();
  EXPECT_EQ(coordinator.Shutdown().state, GameSessionState::kStopped);
  EXPECT_EQ(probe.calls,
            std::vector<std::string>({"foreground", "start", "shutdown"}));
}

TEST(GameSessionCoordinatorTest, UpdateFailureStopsForwardProgress) {
  Probe probe;
  GameSessionCoordinator coordinator(Capabilities(&probe));
  StartJoinFirstOrFail(&coordinator);
  probe.failed_call = "update";

  const GameSessionUpdateResult result =
      coordinator.SurfaceChanged(Surface(33, 0x55, 1600, 900));

  EXPECT_EQ(result.state, GameSessionState::kFailure);
  EXPECT_FALSE(coordinator.LeaveGame().ok());
  EXPECT_EQ(coordinator.Snapshot().active_surface_generation, 33U);
  EXPECT_EQ(coordinator.Snapshot().active_surface_width, 1280U);
  EXPECT_FALSE(coordinator.Snapshot().pending_surface_available);
  EXPECT_EQ(probe.calls,
            std::vector<std::string>({"foreground", "start", "update"}));
}

TEST(GameSessionCoordinatorTest, PauseFailureDoesNotPretendGameIsPaused) {
  Probe probe;
  GameSessionCoordinator coordinator(Capabilities(&probe));
  StartJoinFirstOrFail(&coordinator);
  probe.failed_call = "pause";

  const GameSessionUpdateResult result = coordinator.SurfaceDestroyed(33);

  EXPECT_EQ(result.state, GameSessionState::kFailure);
  EXPECT_FALSE(coordinator.Snapshot().game_paused);
  EXPECT_FALSE(coordinator.Snapshot().active_surface_available);
}

TEST(GameSessionCoordinatorTest, ResumeFailureKeepsSurfaceUnavailable) {
  Probe probe;
  GameSessionCoordinator coordinator(Capabilities(&probe));
  StartJoinFirstOrFail(&coordinator);
  ASSERT_TRUE(coordinator.SurfaceDestroyed(33).ok());
  ASSERT_TRUE(coordinator.SurfaceCreated(34).ok());
  probe.failed_call = "resume";

  const GameSessionUpdateResult result =
      coordinator.SurfaceChanged(Surface(34, 0x55));

  EXPECT_EQ(result.state, GameSessionState::kFailure);
  EXPECT_TRUE(coordinator.Snapshot().game_paused);
  EXPECT_FALSE(coordinator.Snapshot().active_surface_available);
}

TEST(GameSessionCoordinatorTest, LeaveFailureDoesNotClaimLuaAppReturn) {
  Probe probe;
  GameSessionCoordinator coordinator(Capabilities(&probe));
  StartJoinFirstOrFail(&coordinator);
  probe.failed_call = "leave";

  const GameSessionUpdateResult result = coordinator.LeaveGame();

  EXPECT_EQ(result.state, GameSessionState::kFailure);
  EXPECT_FALSE(coordinator.Snapshot().returned_to_lua_app);
}

TEST(GameSessionCoordinatorTest, FailedShutdownIsNeverRetried) {
  Probe probe;
  probe.failed_call = "shutdown";
  GameSessionCoordinator coordinator(Capabilities(&probe));

  const GameSessionUpdateResult first = coordinator.Shutdown();
  const GameSessionUpdateResult repeated = coordinator.Shutdown();

  EXPECT_EQ(first.state, GameSessionState::kFailure);
  EXPECT_EQ(first.cause.code(), StatusCode::kPlatformError);
  EXPECT_EQ(repeated.status, GameSessionUpdateStatus::kRejected);
  EXPECT_EQ(probe.calls, std::vector<std::string>({"shutdown"}));
}

TEST(GameSessionCoordinatorTest, AppShutdownDoesNotInjectLeaveGame) {
  Probe probe;
  GameSessionCoordinator coordinator(Capabilities(&probe));
  StartJoinFirstOrFail(&coordinator);

  const GameSessionUpdateResult result = coordinator.Shutdown();

  EXPECT_EQ(result.state, GameSessionState::kStopped);
  EXPECT_EQ(probe.calls,
            std::vector<std::string>({"foreground", "start", "shutdown"}));
}

TEST(GameSessionCoordinatorTest, NativeLifecycleCallbacksAreSerialized) {
  Probe probe;
  GameSessionCoordinator coordinator(Capabilities(&probe));
  StartJoinFirstOrFail(&coordinator);
  probe.blocked_call = "update";

  GameSessionUpdateResult update_result;
  GameSessionUpdateResult destroy_result;
  std::thread update_thread([&] {
    update_result = coordinator.SurfaceChanged(Surface(33, 0x55, 1600, 900));
  });
  const bool entered = WaitForBlockedCall(&probe);
  if (!entered) {
    ReleaseBlockedCall(&probe);
    update_thread.join();
    FAIL() << "update callback did not enter before timeout";
    return;
  }
  std::thread destroy_thread(
      [&] { destroy_result = coordinator.SurfaceDestroyed(33); });
  ReleaseBlockedCall(&probe);
  update_thread.join();
  destroy_thread.join();

  EXPECT_TRUE(update_result.ok());
  EXPECT_TRUE(destroy_result.ok());
  EXPECT_EQ(destroy_result.state, GameSessionState::kPaused);
  EXPECT_EQ(probe.max_active_callbacks, 1);
  EXPECT_EQ(probe.calls, std::vector<std::string>(
                             {"foreground", "start", "update", "pause"}));
}

TEST(GameSessionCoordinatorTest, StateAndBackendNamesAreStable) {
  EXPECT_STREQ(GameSessionStateName(GameSessionState::kRunning), "Running");
  EXPECT_STREQ(GameSessionStateName(GameSessionState::kPaused), "Paused");
  EXPECT_STREQ(GamePresentBackendName(GamePresentBackend::kVulkan), "Vulkan");
  EXPECT_STREQ(
      GameSessionPrincipalKindName(GameSessionPrincipalKind::kLocalGuest),
      "LocalGuest");
  EXPECT_STREQ(
      GameSessionPrincipalKindName(GameSessionPrincipalKind::kAuthenticated),
      "Authenticated");
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
