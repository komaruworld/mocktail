#include "window/present_observer.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace mocktail {
namespace window {
namespace {

struct PresentProbe {
  int calls = 0;
  uint64_t frame_serial = 0;
};

void RecordPresent(void* context, uint64_t frame_serial) {
  auto* probe = static_cast<PresentProbe*>(context);
  ++probe->calls;
  probe->frame_serial = frame_serial;
}

TEST(PresentObserverRegistryTest, ClearResetsAndAllowsReplacement) {
  PresentObserverRegistry registry;
  PresentProbe first;
  PresentProbe second;

  ASSERT_TRUE(registry.Register(&RecordPresent, &first));
  EXPECT_FALSE(registry.Register(&RecordPresent, &second));
  registry.Notify(7);
  registry.Clear();
  registry.Notify(8);

  EXPECT_EQ(first.calls, 1);
  EXPECT_EQ(first.frame_serial, 7U);
  ASSERT_TRUE(registry.Register(&RecordPresent, &second));
  registry.Notify(9);
  EXPECT_EQ(second.calls, 1);
  EXPECT_EQ(second.frame_serial, 9U);
}

TEST(PresentObserverRegistryTest, RejectsIncompleteRegistration) {
  PresentObserverRegistry registry;
  PresentProbe probe;

  EXPECT_FALSE(registry.Register(nullptr, &probe));
  EXPECT_FALSE(registry.Register(&RecordPresent, nullptr));
}

struct BlockingPresentProbe {
  std::mutex mutex;
  std::condition_variable condition;
  bool callback_entered = false;
  bool release_callback = false;
  bool clear_started = false;
};

void BlockPresent(void* context, uint64_t) {
  auto* probe = static_cast<BlockingPresentProbe*>(context);
  std::unique_lock<std::mutex> lock(probe->mutex);
  probe->callback_entered = true;
  probe->condition.notify_all();
  probe->condition.wait(lock, [probe] { return probe->release_callback; });
}

TEST(PresentObserverRegistryTest, ClearWaitsForInFlightCallback) {
  PresentObserverRegistry registry;
  BlockingPresentProbe probe;
  std::atomic<bool> clear_finished{false};
  ASSERT_TRUE(registry.Register(&BlockPresent, &probe));

  std::thread notifier([&registry] { registry.Notify(11); });
  {
    std::unique_lock<std::mutex> lock(probe.mutex);
    probe.condition.wait(lock, [&probe] { return probe.callback_entered; });
  }
  std::thread clearer([&] {
    {
      std::lock_guard<std::mutex> lock(probe.mutex);
      probe.clear_started = true;
      probe.condition.notify_all();
    }
    registry.Clear();
    clear_finished.store(true, std::memory_order_release);
  });
  {
    std::unique_lock<std::mutex> lock(probe.mutex);
    probe.condition.wait(lock, [&probe] { return probe.clear_started; });
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_FALSE(clear_finished.load(std::memory_order_acquire));

  {
    std::lock_guard<std::mutex> lock(probe.mutex);
    probe.release_callback = true;
    probe.condition.notify_all();
  }
  notifier.join();
  clearer.join();
  EXPECT_TRUE(clear_finished.load(std::memory_order_acquire));
}

enum class LifecycleOperation {
  kClear,
  kDeactivate,
};

void ExpectOperationWaitsForPostObserverPresentWork(
    LifecycleOperation operation) {
  PresentLifecycleGate gate;
  PresentProbe probe;
  ASSERT_TRUE(gate.Activate());
  ASSERT_TRUE(gate.Register(&RecordPresent, &probe));

  std::mutex mutex;
  std::condition_variable condition;
  bool post_observer_work_entered = false;
  bool operation_started = false;
  bool finish_present_work = false;
  std::atomic<int> sequence{0};
  int present_work_finished = 0;
  int operation_finished = 0;

  std::thread presenter([&] {
    {
      auto present_scope = gate.EnterPresent();
      if (!present_scope) {
        return;
      }
      present_scope.NotifyObserver(23);
      {
        std::unique_lock<std::mutex> lock(mutex);
        post_observer_work_entered = true;
        condition.notify_all();
        condition.wait(lock, [&] { return finish_present_work; });
      }
      present_work_finished =
          sequence.fetch_add(1, std::memory_order_acq_rel) + 1;
    }
  });

  {
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait(lock, [&] { return post_observer_work_entered; });
  }
  std::thread lifecycle_operation([&] {
    {
      std::lock_guard<std::mutex> lock(mutex);
      operation_started = true;
      condition.notify_all();
    }
    if (operation == LifecycleOperation::kClear) {
      gate.Clear();
    } else {
      gate.Deactivate();
    }
    operation_finished = sequence.fetch_add(1, std::memory_order_acq_rel) + 1;
  });
  {
    std::unique_lock<std::mutex> lock(mutex);
    condition.wait(lock, [&] { return operation_started; });
    finish_present_work = true;
    condition.notify_all();
  }

  presenter.join();
  lifecycle_operation.join();

  EXPECT_EQ(probe.calls, 1);
  EXPECT_EQ(probe.frame_serial, 23U);
  EXPECT_GT(present_work_finished, 0);
  EXPECT_GT(operation_finished, present_work_finished);

  PresentProbe replacement;
  if (operation == LifecycleOperation::kClear) {
    ASSERT_TRUE(gate.Register(&RecordPresent, &replacement));
    auto present_scope = gate.EnterPresent();
    ASSERT_TRUE(present_scope);
    present_scope.NotifyObserver(24);
    EXPECT_EQ(replacement.calls, 1);
    EXPECT_EQ(replacement.frame_serial, 24U);
  } else {
    EXPECT_FALSE(gate.EnterPresent());
    EXPECT_FALSE(gate.Register(&RecordPresent, &replacement));
  }
}

TEST(PresentLifecycleGateTest, ClearWaitsForPostObserverPresentWork) {
  ExpectOperationWaitsForPostObserverPresentWork(LifecycleOperation::kClear);
}

TEST(PresentLifecycleGateTest,
     DeactivateWaitsForPostObserverPresentWork) {
  ExpectOperationWaitsForPostObserverPresentWork(
      LifecycleOperation::kDeactivate);
}

TEST(PresentLifecycleGateTest, DeactivateClearsObserverBeforeReinit) {
  PresentLifecycleGate gate;
  PresentProbe first;
  PresentProbe second;

  EXPECT_FALSE(gate.Register(&RecordPresent, &first));
  EXPECT_FALSE(gate.EnterPresent());

  ASSERT_TRUE(gate.Activate());
  ASSERT_TRUE(gate.Register(&RecordPresent, &first));
  {
    auto present_scope = gate.EnterPresent();
    ASSERT_TRUE(present_scope);
    present_scope.NotifyObserver(31);
  }
  gate.Deactivate();

  ASSERT_TRUE(gate.Activate());
  ASSERT_TRUE(gate.Register(&RecordPresent, &second));
  {
    auto present_scope = gate.EnterPresent();
    ASSERT_TRUE(present_scope);
    present_scope.NotifyObserver(32);
  }

  EXPECT_EQ(first.calls, 1);
  EXPECT_EQ(first.frame_serial, 31U);
  EXPECT_EQ(second.calls, 1);
  EXPECT_EQ(second.frame_serial, 32U);
}

}  // namespace
}  // namespace window
}  // namespace mocktail
