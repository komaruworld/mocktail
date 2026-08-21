#include "window/window_resize_readiness_gate.h"

#include <gtest/gtest.h>

namespace mocktail {
namespace window {
namespace {

WindowSurfaceSnapshot InitialSurface() {
  return {1, 0x44, 1280, 720, true};
}

WindowSurfaceEvent Changed(uint64_t generation, uint32_t width,
                           uint32_t height) {
  return {WindowSurfaceEventType::kChanged,
          {generation, 0x44, width, height, true}};
}

TEST(WindowResizeReadinessGateTest, DisabledGateIsCompleteAndDoesNothing) {
  WindowResizeReadinessGate gate;

  ASSERT_TRUE(gate.Activate({}, {}).ok());
  ASSERT_TRUE(gate.RecordPresent(1).ok());
  ASSERT_TRUE(gate.RecordCommittedSurfaceEvent(Changed(1, 1600, 900)).ok());
  ASSERT_TRUE(gate.RecordStopped().ok());

  EXPECT_TRUE(gate.CompletionStatus().ok());
  EXPECT_TRUE(gate.Snapshot().complete());
  EXPECT_EQ(gate.Snapshot().state, WindowResizeReadinessState::kDisabled);
}

TEST(WindowResizeReadinessGateTest, RequiresCompleteRealEvidenceInOrder) {
  WindowResizeReadinessGate gate;
  ASSERT_TRUE(gate.Activate({true, 1600, 900}, InitialSurface()).ok());
  WindowResizeRequest request;

  EXPECT_FALSE(gate.TakeResizeRequest(&request));
  ASSERT_TRUE(gate.RecordPresent(4).ok());
  ASSERT_TRUE(gate.TakeResizeRequest(&request));
  EXPECT_EQ(request.width, 1600U);
  EXPECT_EQ(request.height, 900U);
  EXPECT_FALSE(gate.TakeResizeRequest(&request));
  ASSERT_TRUE(gate.RecordCommittedSurfaceEvent(Changed(1, 1600, 900)).ok());
  ASSERT_TRUE(gate.RecordPresent(5).ok());
  ASSERT_TRUE(gate.RecordStopped().ok());

  const WindowResizeReadinessSnapshot snapshot = gate.Snapshot();
  EXPECT_EQ(snapshot.state, WindowResizeReadinessState::kStopped);
  EXPECT_EQ(snapshot.surface_generation, 1U);
  EXPECT_EQ(snapshot.first_present_serial, 4U);
  EXPECT_EQ(snapshot.post_rebind_present_serial, 5U);
  EXPECT_TRUE(snapshot.complete());
  EXPECT_TRUE(gate.CompletionStatus().ok());
}

TEST(WindowResizeReadinessGateTest, IntermediateConfigureIsNotAcceptedAsTarget) {
  WindowResizeReadinessGate gate;
  ASSERT_TRUE(gate.Activate({true, 1600, 900}, InitialSurface()).ok());
  ASSERT_TRUE(gate.RecordPresent(2).ok());
  WindowResizeRequest request;
  ASSERT_TRUE(gate.TakeResizeRequest(&request));

  ASSERT_TRUE(gate.RecordCommittedSurfaceEvent(Changed(1, 1500, 850)).ok());
  EXPECT_EQ(gate.Snapshot().state,
            WindowResizeReadinessState::kResizeRequested);
  ASSERT_TRUE(gate.RecordPresent(3).ok());
  EXPECT_EQ(gate.Snapshot().state,
            WindowResizeReadinessState::kResizeRequested);
  EXPECT_FALSE(gate.CompletionStatus().ok());

  ASSERT_TRUE(gate.RecordCommittedSurfaceEvent(Changed(1, 1600, 900)).ok());
  EXPECT_EQ(gate.Snapshot().state,
            WindowResizeReadinessState::kSurfaceCommitted);
}

TEST(WindowResizeReadinessGateTest, RecreatedSurfaceFailsSameGenerationGate) {
  WindowResizeReadinessGate gate;
  ASSERT_TRUE(gate.Activate({true, 1600, 900}, InitialSurface()).ok());
  ASSERT_TRUE(gate.RecordPresent(2).ok());
  WindowResizeRequest request;
  ASSERT_TRUE(gate.TakeResizeRequest(&request));

  const Status status = gate.RecordCommittedSurfaceEvent(
      {WindowSurfaceEventType::kDestroyed, {1, 0x44, 1280, 720, false}});

  EXPECT_FALSE(status.ok());
  EXPECT_EQ(gate.Snapshot().state, WindowResizeReadinessState::kFailed);
  EXPECT_FALSE(gate.CompletionStatus().ok());
}

TEST(WindowResizeReadinessGateTest, ChangedNewGenerationFailsGate) {
  WindowResizeReadinessGate gate;
  ASSERT_TRUE(gate.Activate({true, 1600, 900}, InitialSurface()).ok());
  ASSERT_TRUE(gate.RecordPresent(2).ok());
  WindowResizeRequest request;
  ASSERT_TRUE(gate.TakeResizeRequest(&request));

  const Status status =
      gate.RecordCommittedSurfaceEvent(Changed(2, 1600, 900));

  EXPECT_FALSE(status.ok());
  EXPECT_EQ(gate.Snapshot().state, WindowResizeReadinessState::kFailed);
}

TEST(WindowResizeReadinessGateTest, StopBeforeLaterPresentFailsClosed) {
  WindowResizeReadinessGate gate;
  ASSERT_TRUE(gate.Activate({true, 1600, 900}, InitialSurface()).ok());
  ASSERT_TRUE(gate.RecordPresent(7).ok());
  WindowResizeRequest request;
  ASSERT_TRUE(gate.TakeResizeRequest(&request));
  ASSERT_TRUE(gate.RecordCommittedSurfaceEvent(Changed(1, 1600, 900)).ok());

  const Status status = gate.RecordStopped();

  EXPECT_FALSE(status.ok());
  EXPECT_EQ(gate.Snapshot().state, WindowResizeReadinessState::kFailed);
  EXPECT_FALSE(gate.CompletionStatus().ok());
}

TEST(WindowResizeReadinessGateTest, RejectsInvalidActivationAndPresentShape) {
  WindowResizeReadinessGate missing_surface;
  WindowResizeReadinessGate unchanged_target;
  WindowResizeReadinessGate invalid_present;

  EXPECT_FALSE(missing_surface.Activate({true, 1600, 900}, {}).ok());
  EXPECT_FALSE(
      unchanged_target.Activate({true, 1280, 720}, InitialSurface()).ok());
  ASSERT_TRUE(
      invalid_present.Activate({true, 1600, 900}, InitialSurface()).ok());
  EXPECT_FALSE(invalid_present.RecordPresent(0).ok());
  EXPECT_EQ(invalid_present.Snapshot().state,
            WindowResizeReadinessState::kFailed);
}

}  // namespace
}  // namespace window
}  // namespace mocktail
