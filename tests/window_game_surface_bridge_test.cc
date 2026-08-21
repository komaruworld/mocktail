#include "window/window_game_surface_bridge.h"

#include <gtest/gtest.h>

#include <deque>
#include <vector>

namespace mocktail {
namespace window {
namespace {

runtime::GameSessionUpdateResult Applied() {
  return {runtime::GameSessionUpdateStatus::kApplied,
          runtime::GameSessionState::kRunning, Status::Ok(), "applied"};
}

runtime::GameSessionUpdateResult Duplicate() {
  return {runtime::GameSessionUpdateStatus::kDuplicate,
          runtime::GameSessionState::kRunning, Status::Ok(), "duplicate"};
}

runtime::GameSessionUpdateResult Rejected(const char* message) {
  return {runtime::GameSessionUpdateStatus::kRejected,
          runtime::GameSessionState::kFailure,
          Status::Error(StatusCode::kFailedPrecondition, message), message};
}

struct SourceProbe {
  std::deque<WindowSurfaceEvent> events;
  int polls = 0;
};

bool Poll(void* context, WindowSurfaceEvent* event) {
  auto* probe = static_cast<SourceProbe*>(context);
  ++probe->polls;
  if (probe->events.empty()) {
    return false;
  }
  *event = probe->events.front();
  probe->events.pop_front();
  return true;
}

struct ConsumerProbe {
  std::vector<WindowSurfaceEventType> calls;
  std::vector<runtime::GameSurface> changed_surfaces;
  bool reject_changed = false;
  bool duplicate_created = false;
  int commits = 0;
  bool reject_commit = false;
};

Status Committed(void* context, const WindowSurfaceEvent&) {
  auto* probe = static_cast<ConsumerProbe*>(context);
  ++probe->commits;
  if (probe->reject_commit) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "readiness commit rejected");
  }
  return Status::Ok();
}

runtime::GameSessionUpdateResult Created(void* context, uint64_t generation) {
  auto* probe = static_cast<ConsumerProbe*>(context);
  probe->calls.push_back(WindowSurfaceEventType::kCreated);
  EXPECT_NE(generation, 0U);
  return probe->duplicate_created ? Duplicate() : Applied();
}

runtime::GameSessionUpdateResult Changed(void* context,
                                         runtime::GameSurface surface) {
  auto* probe = static_cast<ConsumerProbe*>(context);
  probe->calls.push_back(WindowSurfaceEventType::kChanged);
  probe->changed_surfaces.push_back(surface);
  return probe->reject_changed ? Rejected("native update rejected") : Applied();
}

runtime::GameSessionUpdateResult Destroyed(void* context,
                                           uint64_t generation) {
  auto* probe = static_cast<ConsumerProbe*>(context);
  probe->calls.push_back(WindowSurfaceEventType::kDestroyed);
  EXPECT_NE(generation, 0U);
  return Applied();
}

WindowSurfaceEvent Event(WindowSurfaceEventType type, uint64_t generation,
                         uintptr_t native_window, uint32_t width,
                         uint32_t height, bool available) {
  return {type,
          {generation, native_window, width, height, available}};
}

TEST(WindowGameSurfaceBridgeTest, DrainsEventsInOrderAndPreservesSurfaceData) {
  SourceProbe source{{
      Event(WindowSurfaceEventType::kCreated, 2, 0x55, 1600, 900, true),
      Event(WindowSurfaceEventType::kChanged, 2, 0x55, 1600, 900, true),
      Event(WindowSurfaceEventType::kDestroyed, 2, 0x55, 1600, 900, false),
  }};
  ConsumerProbe consumer;
  WindowGameSurfaceBridge bridge(
      {&source, &Poll},
      {&consumer, &Created, &Changed, &Destroyed},
      {&consumer, &Committed});

  const WindowGameSurfaceDrainResult result = bridge.Drain();

  ASSERT_TRUE(result.ok()) << result.status.message();
  EXPECT_EQ(result.drained_events, 3U);
  EXPECT_EQ(consumer.calls,
            (std::vector<WindowSurfaceEventType>{
                WindowSurfaceEventType::kCreated,
                WindowSurfaceEventType::kChanged,
                WindowSurfaceEventType::kDestroyed}));
  ASSERT_EQ(consumer.changed_surfaces.size(), 1U);
  EXPECT_EQ(consumer.changed_surfaces[0].generation, 2U);
  EXPECT_EQ(consumer.changed_surfaces[0].native_window, 0x55U);
  EXPECT_EQ(consumer.changed_surfaces[0].width, 1600U);
  EXPECT_EQ(consumer.changed_surfaces[0].height, 900U);
  const WindowGameSurfaceBridgeSnapshot snapshot = bridge.Snapshot();
  EXPECT_EQ(snapshot.accepted_events, 3U);
  EXPECT_EQ(consumer.commits, 3);
  EXPECT_EQ(snapshot.duplicate_events, 0U);
  EXPECT_EQ(snapshot.last_generation, 2U);
  EXPECT_FALSE(snapshot.failed);
}

TEST(WindowGameSurfaceBridgeTest, CountsAcceptedDuplicateWithoutFailure) {
  SourceProbe source{{
      Event(WindowSurfaceEventType::kCreated, 2, 0x55, 1600, 900, true),
  }};
  ConsumerProbe consumer;
  consumer.duplicate_created = true;
  WindowGameSurfaceBridge bridge(
      {&source, &Poll},
      {&consumer, &Created, &Changed, &Destroyed});

  ASSERT_TRUE(bridge.Drain().ok());

  const WindowGameSurfaceBridgeSnapshot snapshot = bridge.Snapshot();
  EXPECT_EQ(snapshot.accepted_events, 0U);
  EXPECT_EQ(snapshot.duplicate_events, 1U);
  EXPECT_FALSE(snapshot.failed);
}

TEST(WindowGameSurfaceBridgeTest, RejectionIsTerminalAndLeavesLaterEventQueued) {
  SourceProbe source{{
      Event(WindowSurfaceEventType::kChanged, 1, 0x44, 1600, 900, true),
      Event(WindowSurfaceEventType::kDestroyed, 1, 0x44, 1600, 900, false),
  }};
  ConsumerProbe consumer;
  consumer.reject_changed = true;
  WindowGameSurfaceBridge bridge(
      {&source, &Poll},
      {&consumer, &Created, &Changed, &Destroyed});

  const WindowGameSurfaceDrainResult first = bridge.Drain();
  const int polls_after_failure = source.polls;
  const WindowGameSurfaceDrainResult second = bridge.Drain();

  EXPECT_FALSE(first.ok());
  EXPECT_EQ(first.drained_events, 1U);
  EXPECT_EQ(first.status.message(), "native update rejected");
  EXPECT_FALSE(second.ok());
  EXPECT_EQ(second.drained_events, 0U);
  EXPECT_EQ(source.polls, polls_after_failure);
  EXPECT_EQ(source.events.size(), 1U);
  EXPECT_TRUE(bridge.Snapshot().failed);
}

TEST(WindowGameSurfaceBridgeTest, InvalidChangedEventFailsBeforeNativeDispatch) {
  SourceProbe source{{
      Event(WindowSurfaceEventType::kChanged, 1, 0, 1600, 900, true),
  }};
  ConsumerProbe consumer;
  WindowGameSurfaceBridge bridge(
      {&source, &Poll},
      {&consumer, &Created, &Changed, &Destroyed});

  const WindowGameSurfaceDrainResult result = bridge.Drain();

  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.status.code(), StatusCode::kInvalidArgument);
  EXPECT_TRUE(consumer.calls.empty());
}

TEST(WindowGameSurfaceBridgeTest, CommitObserverRunsOnlyAfterNativeAcceptance) {
  SourceProbe source{{
      Event(WindowSurfaceEventType::kChanged, 1, 0x44, 1600, 900, true),
  }};
  ConsumerProbe consumer;
  consumer.reject_changed = true;
  WindowGameSurfaceBridge rejected_bridge(
      {&source, &Poll}, {&consumer, &Created, &Changed, &Destroyed},
      {&consumer, &Committed});

  EXPECT_FALSE(rejected_bridge.Drain().ok());
  EXPECT_EQ(consumer.commits, 0);

  SourceProbe accepted_source{{
      Event(WindowSurfaceEventType::kChanged, 1, 0x44, 1600, 900, true),
  }};
  ConsumerProbe accepted_consumer;
  accepted_consumer.reject_commit = true;
  WindowGameSurfaceBridge accepted_bridge(
      {&accepted_source, &Poll},
      {&accepted_consumer, &Created, &Changed, &Destroyed},
      {&accepted_consumer, &Committed});

  const WindowGameSurfaceDrainResult result = accepted_bridge.Drain();
  EXPECT_FALSE(result.ok());
  EXPECT_EQ(result.status.message(), "readiness commit rejected");
  EXPECT_EQ(accepted_consumer.calls.size(), 1U);
  EXPECT_EQ(accepted_consumer.commits, 1);
}

TEST(WindowGameSurfaceBridgeTest, IncompleteBindingsFailClosed) {
  SourceProbe source;
  ConsumerProbe consumer;
  WindowGameSurfaceBridge missing_source(
      {}, {&consumer, &Created, &Changed, &Destroyed});
  WindowGameSurfaceBridge missing_consumer({&source, &Poll}, {});

  EXPECT_FALSE(missing_source.Drain().ok());
  EXPECT_FALSE(missing_consumer.Drain().ok());
  EXPECT_TRUE(missing_source.Snapshot().failed);
  EXPECT_TRUE(missing_consumer.Snapshot().failed);
}

}  // namespace
}  // namespace window
}  // namespace mocktail
