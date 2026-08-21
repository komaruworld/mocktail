#include "window/window_surface_lifecycle.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include "window/platform_event_observer.h"

namespace mocktail {
namespace window {
namespace {

std::vector<WindowSurfaceEvent> Drain(WindowSurfaceLifecycle* lifecycle) {
  std::vector<WindowSurfaceEvent> events;
  WindowSurfaceEvent event;
  while (lifecycle != nullptr && lifecycle->Poll(&event)) {
    events.push_back(event);
  }
  return events;
}

struct ObserverProbe {
  std::mutex mutex;
  std::condition_variable condition;
  bool entered = false;
  bool release = false;
  int calls = 0;
};

void BlockingObserver(void* context, const platform::PlatformEvent&) {
  auto* probe = static_cast<ObserverProbe*>(context);
  std::unique_lock<std::mutex> lock(probe->mutex);
  ++probe->calls;
  probe->entered = true;
  probe->condition.notify_all();
  probe->condition.wait_for(lock, std::chrono::seconds(2),
                            [probe] { return probe->release; });
}

TEST(WindowSurfaceLifecycleTest, InitialSurfaceIsGenerationOneWithoutEvent) {
  WindowSurfaceLifecycle lifecycle;

  const Status status = lifecycle.Activate(0x44, 1280, 720);

  ASSERT_TRUE(status.ok()) << status.message();
  const WindowSurfaceSnapshot snapshot = lifecycle.Snapshot();
  EXPECT_EQ(snapshot.generation, 1U);
  EXPECT_EQ(snapshot.native_window, 0x44U);
  EXPECT_EQ(snapshot.width, 1280U);
  EXPECT_EQ(snapshot.height, 720U);
  EXPECT_TRUE(snapshot.available);
  EXPECT_TRUE(Drain(&lifecycle).empty());
}

TEST(WindowSurfaceLifecycleTest, SameHandleResizeKeepsGenerationAndCoalesces) {
  WindowSurfaceLifecycle lifecycle;
  ASSERT_TRUE(lifecycle.Activate(0x44, 1280, 720).ok());

  ASSERT_TRUE(lifecycle.Observe(0x44, 1400, 800).ok());
  ASSERT_TRUE(lifecycle.Observe(0x44, 1600, 900).ok());

  const std::vector<WindowSurfaceEvent> events = Drain(&lifecycle);
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].type, WindowSurfaceEventType::kChanged);
  EXPECT_EQ(events[0].surface.generation, 1U);
  EXPECT_EQ(events[0].surface.native_window, 0x44U);
  EXPECT_EQ(events[0].surface.width, 1600U);
  EXPECT_EQ(events[0].surface.height, 900U);
}

TEST(WindowSurfaceLifecycleTest,
     RefreshQueuesSameExtentChangedEventAndCoalesces) {
  WindowSurfaceLifecycle lifecycle;
  ASSERT_TRUE(lifecycle.Activate(0x44, 1280, 720).ok());

  ASSERT_TRUE(lifecycle.Refresh().ok());
  ASSERT_TRUE(lifecycle.Refresh().ok());

  const std::vector<WindowSurfaceEvent> events = Drain(&lifecycle);
  ASSERT_EQ(events.size(), 1U);
  EXPECT_EQ(events[0].type, WindowSurfaceEventType::kChanged);
  EXPECT_EQ(events[0].surface.generation, 1U);
  EXPECT_EQ(events[0].surface.native_window, 0x44U);
  EXPECT_EQ(events[0].surface.width, 1280U);
  EXPECT_EQ(events[0].surface.height, 720U);
}

TEST(WindowSurfaceLifecycleTest,
     RecreateQueuesDestroyedCreatedChangedWithNewGeneration) {
  WindowSurfaceLifecycle lifecycle;
  ASSERT_TRUE(lifecycle.Activate(0x44, 1280, 720).ok());

  ASSERT_TRUE(lifecycle.Recreate().ok());

  const std::vector<WindowSurfaceEvent> events = Drain(&lifecycle);
  ASSERT_EQ(events.size(), 3U);
  EXPECT_EQ(events[0].type, WindowSurfaceEventType::kDestroyed);
  EXPECT_EQ(events[0].surface.generation, 1U);
  EXPECT_EQ(events[0].surface.native_window, 0x44U);
  EXPECT_FALSE(events[0].surface.available);
  EXPECT_EQ(events[1].type, WindowSurfaceEventType::kCreated);
  EXPECT_EQ(events[1].surface.generation, 2U);
  EXPECT_TRUE(events[1].surface.available);
  EXPECT_EQ(events[2].type, WindowSurfaceEventType::kChanged);
  EXPECT_EQ(events[2].surface.generation, 2U);
  EXPECT_EQ(events[2].surface.native_window, 0x44U);
  EXPECT_EQ(events[2].surface.width, 1280U);
  EXPECT_EQ(events[2].surface.height, 720U);
  EXPECT_EQ(lifecycle.Snapshot().generation, 2U);
}

TEST(WindowSurfaceLifecycleTest, ZeroExtentDoesNotFabricateDestruction) {
  WindowSurfaceLifecycle lifecycle;
  ASSERT_TRUE(lifecycle.Activate(0x44, 1280, 720).ok());

  ASSERT_TRUE(lifecycle.Observe(0x44, 0, 0).ok());

  EXPECT_TRUE(Drain(&lifecycle).empty());
  const WindowSurfaceSnapshot snapshot = lifecycle.Snapshot();
  EXPECT_TRUE(snapshot.available);
  EXPECT_EQ(snapshot.generation, 1U);
  EXPECT_EQ(snapshot.width, 1280U);
  EXPECT_EQ(snapshot.height, 720U);
}

TEST(WindowSurfaceLifecycleTest, HandleLossAndRestoreCreatesNewGeneration) {
  WindowSurfaceLifecycle lifecycle;
  ASSERT_TRUE(lifecycle.Activate(0x44, 1280, 720).ok());

  ASSERT_TRUE(lifecycle.Observe(0, 0, 0).ok());
  ASSERT_TRUE(lifecycle.Observe(0x44, 1920, 1080).ok());

  const std::vector<WindowSurfaceEvent> events = Drain(&lifecycle);
  ASSERT_EQ(events.size(), 3U);
  EXPECT_EQ(events[0].type, WindowSurfaceEventType::kDestroyed);
  EXPECT_EQ(events[0].surface.generation, 1U);
  EXPECT_EQ(events[1].type, WindowSurfaceEventType::kCreated);
  EXPECT_EQ(events[1].surface.generation, 2U);
  EXPECT_EQ(events[2].type, WindowSurfaceEventType::kChanged);
  EXPECT_EQ(events[2].surface.generation, 2U);
  EXPECT_EQ(events[2].surface.width, 1920U);
  EXPECT_TRUE(lifecycle.Snapshot().available);
}

TEST(WindowSurfaceLifecycleTest, HandleReplacementDestroysBeforeRecreate) {
  WindowSurfaceLifecycle lifecycle;
  ASSERT_TRUE(lifecycle.Activate(0x44, 1280, 720).ok());

  ASSERT_TRUE(lifecycle.Observe(0x55, 1280, 720).ok());

  const std::vector<WindowSurfaceEvent> events = Drain(&lifecycle);
  ASSERT_EQ(events.size(), 3U);
  EXPECT_EQ(events[0].type, WindowSurfaceEventType::kDestroyed);
  EXPECT_EQ(events[0].surface.native_window, 0x44U);
  EXPECT_EQ(events[1].type, WindowSurfaceEventType::kCreated);
  EXPECT_EQ(events[1].surface.native_window, 0x55U);
  EXPECT_EQ(events[1].surface.generation, 2U);
  EXPECT_EQ(events[2].type, WindowSurfaceEventType::kChanged);
}

TEST(WindowSurfaceLifecycleTest, DeactivateDrainsAndRejectsFurtherEvents) {
  WindowSurfaceLifecycle lifecycle;
  ASSERT_TRUE(lifecycle.Activate(0x44, 1280, 720).ok());
  ASSERT_TRUE(lifecycle.Observe(0x44, 1600, 900).ok());

  lifecycle.Deactivate();

  EXPECT_FALSE(lifecycle.active());
  EXPECT_TRUE(Drain(&lifecycle).empty());
  EXPECT_FALSE(lifecycle.Observe(0x44, 1920, 1080).ok());
  EXPECT_FALSE(lifecycle.Refresh().ok());
  EXPECT_FALSE(lifecycle.Recreate().ok());
  EXPECT_FALSE(lifecycle.Snapshot().available);
}

TEST(PlatformEventObserverGateTest, ClearWaitsForInFlightCallback) {
  PlatformEventObserverGate gate;
  ObserverProbe probe;
  ASSERT_TRUE(gate.Activate());
  ASSERT_TRUE(gate.Register(BlockingObserver, &probe));
  platform::PlatformEvent event;
  std::thread notify_thread([&] { gate.Notify(event); });
  bool entered = false;
  {
    std::unique_lock<std::mutex> lock(probe.mutex);
    entered = probe.condition.wait_for(lock, std::chrono::seconds(2),
                                       [&probe] { return probe.entered; });
  }
  if (!entered) {
    {
      std::lock_guard<std::mutex> lock(probe.mutex);
      probe.release = true;
    }
    probe.condition.notify_all();
    notify_thread.join();
    FAIL() << "observer callback did not enter";
    return;
  }

  std::atomic<bool> clear_started{false};
  std::atomic<bool> clear_completed{false};
  std::thread clear_thread([&] {
    clear_started.store(true, std::memory_order_release);
    gate.Clear();
    clear_completed.store(true, std::memory_order_release);
  });
  while (!clear_started.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  EXPECT_FALSE(clear_completed.load(std::memory_order_acquire));
  {
    std::lock_guard<std::mutex> lock(probe.mutex);
    probe.release = true;
  }
  probe.condition.notify_all();
  notify_thread.join();
  clear_thread.join();

  EXPECT_TRUE(clear_completed.load(std::memory_order_acquire));
  gate.Notify(event);
  EXPECT_EQ(probe.calls, 1);
}

TEST(PlatformEventObserverGateTest, DeactivateClearsRegistration) {
  PlatformEventObserverGate gate;
  ObserverProbe probe;
  ASSERT_TRUE(gate.Activate());
  ASSERT_TRUE(gate.Register(BlockingObserver, &probe));

  gate.Deactivate();

  EXPECT_FALSE(gate.HasObserver());
  EXPECT_TRUE(gate.Activate());
  EXPECT_TRUE(gate.Register(BlockingObserver, &probe));
  gate.Clear();
}

}  // namespace
}  // namespace window
}  // namespace mocktail
