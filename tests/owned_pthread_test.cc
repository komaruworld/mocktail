#include "runtime/owned_pthread.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace mocktail {
namespace runtime {
namespace {

void* CompleteImmediately(void* context) {
  static_cast<std::atomic<bool>*>(context)->store(true,
                                                  std::memory_order_release);
  return nullptr;
}

TEST(OwnedPthreadTest, StartsAndPhysicallyJoinsWorker) {
  std::atomic<bool> completed{false};
  OwnedPthread worker;

  ASSERT_EQ(worker.Start(&CompleteImmediately, &completed, 0), 0);
  const OwnedPthreadWaitResult result = worker.WaitFor(1000, 1);

  EXPECT_TRUE(result.joined());
  EXPECT_TRUE(completed.load(std::memory_order_acquire));
  EXPECT_FALSE(worker.joinable());
}

struct PumpProbe {
  std::atomic<bool> release{false};
  int calls = 0;
};

void* WaitForPump(void* context) {
  auto* probe = static_cast<PumpProbe*>(context);
  while (!probe->release.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  return nullptr;
}

void ReleaseFromPump(void* context) {
  auto* probe = static_cast<PumpProbe*>(context);
  ++probe->calls;
  if (probe->calls == 3) {
    probe->release.store(true, std::memory_order_release);
  }
}

TEST(OwnedPthreadTest, PumpsOwnerThreadWhileWaiting) {
  PumpProbe probe;
  OwnedPthread worker;
  ASSERT_EQ(worker.Start(&WaitForPump, &probe, 0), 0);

  const OwnedPthreadWaitResult result =
      worker.WaitFor(1000, 1, &ReleaseFromPump, &probe);

  EXPECT_TRUE(result.joined());
  EXPECT_GE(probe.calls, 3);
}

void* WaitAtCancellationPoint(void* context) {
  auto* entered = static_cast<std::atomic<bool>*>(context);
  entered->store(true, std::memory_order_release);
  while (true) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    pthread_testcancel();
  }
}

TEST(OwnedPthreadTest, CancellationStillRequiresPhysicalJoin) {
  std::atomic<bool> entered{false};
  OwnedPthread worker;
  ASSERT_EQ(worker.Start(&WaitAtCancellationPoint, &entered, 0), 0);
  while (!entered.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  const OwnedPthreadCancelResult result = worker.CancelAndJoinFor(1000, 1);

  EXPECT_EQ(result.cancel_error, 0);
  EXPECT_TRUE(result.wait.joined());
  EXPECT_FALSE(worker.joinable());
}

struct DeferredCancellationProbe {
  std::atomic<bool> entered{false};
  std::atomic<bool> release{false};
};

void* IgnoreCancellationUntilReleased(void* context) {
  auto* probe = static_cast<DeferredCancellationProbe*>(context);
  pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, nullptr);
  probe->entered.store(true, std::memory_order_release);
  while (!probe->release.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return nullptr;
}

TEST(OwnedPthreadTest, DeferredCancellationNeverConvertsToDetach) {
  DeferredCancellationProbe probe;
  OwnedPthread worker;
  ASSERT_EQ(worker.Start(&IgnoreCancellationUntilReleased, &probe, 0), 0);
  while (!probe.entered.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  const auto cancel_started_at = std::chrono::steady_clock::now();
  const OwnedPthreadCancelResult cancellation = worker.CancelAndJoinFor(20, 1);
  const auto cancel_elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - cancel_started_at);

  EXPECT_EQ(cancellation.cancel_error, 0);
  EXPECT_EQ(cancellation.wait.status, OwnedPthreadWaitStatus::kTimedOut);
  EXPECT_GE(cancel_elapsed.count(), 15);
  EXPECT_TRUE(worker.joinable());
  probe.release.store(true, std::memory_order_release);
  EXPECT_TRUE(worker.WaitFor(1000, 1).joined());
}

TEST(OwnedPthreadTest, WaitStatusNamesAreStable) {
  EXPECT_STREQ(OwnedPthreadWaitStatusName(OwnedPthreadWaitStatus::kJoined),
               "Joined");
  EXPECT_STREQ(OwnedPthreadWaitStatusName(OwnedPthreadWaitStatus::kTimedOut),
               "TimedOut");
  EXPECT_STREQ(
      OwnedPthreadWaitStatusName(OwnedPthreadWaitStatus::kPlatformError),
      "PlatformError");
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
