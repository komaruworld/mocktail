#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "mocktail/vr/openxr_backend.h"

using SubmitAdapter = VkResult(VKAPI_PTR *)(PFN_vkQueueSubmit, VkDevice,
                                            VkQueue, uint32_t,
                                            const VkSubmitInfo *, VkFence);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL mocktail_vulkan_submit_synchronized(
    PFN_vkQueueSubmit raw, VkDevice device, VkQueue queue, uint32_t count,
    const VkSubmitInfo *submits, VkFence fence);
extern "C" void mocktail_vr_xr_queue_submit_adapter(SubmitAdapter);
extern "C" VKAPI_ATTR VkResult VKAPI_CALL
mocktail_vulkan_idle_synchronized(PFN_vkDeviceWaitIdle raw, VkDevice device);

namespace {
std::mutex state_mutex;
std::condition_variable state_changed;
bool waiting, signal_submitted;
bool idle_release;
std::atomic<int> inside_driver{0}, max_inside{0};
VKAPI_ATTR VkResult VKAPI_CALL Reset(VkDevice, uint32_t, const VkFence *) {
  return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL Submit(VkQueue, uint32_t,
                                      const VkSubmitInfo *info, VkFence) {
  if (info && info->signalSemaphoreCount) {
    std::lock_guard<std::mutex> lock(state_mutex);
    signal_submitted = true;
    state_changed.notify_all();
  }
  return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL Wait(VkDevice, uint32_t, const VkFence *,
                                    VkBool32, uint64_t) {
  std::unique_lock<std::mutex> lock(state_mutex);
  waiting = true;
  state_changed.notify_all();
  return state_changed.wait_for(lock, std::chrono::seconds(2),
                                [] { return signal_submitted; })
             ? VK_SUCCESS
             : VK_TIMEOUT;
}
VKAPI_ATTR VkResult VKAPI_CALL SlowSubmit(VkQueue, uint32_t,
                                          const VkSubmitInfo *, VkFence) {
  const int active = ++inside_driver;
  int previous = max_inside.load();
  while (active > previous &&
         !max_inside.compare_exchange_weak(previous, active)) {
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  --inside_driver;
  return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL BlockingIdle(VkDevice) {
  std::unique_lock<std::mutex> lock(state_mutex);
  waiting = true;
  state_changed.notify_all();
  return state_changed.wait_for(lock, std::chrono::seconds(2),
                                [] { return idle_release; })
             ? VK_SUCCESS
             : VK_TIMEOUT;
}
TEST(VrQueueSubmission, DeviceIdleExcludesPrivateSubmitsUntilDriverReturns) {
  waiting = signal_submitted = idle_release = false;
  VkResult idle_result = VK_NOT_READY;
  std::thread idler([&] {
    idle_result = mocktail_vulkan_idle_synchronized(
        BlockingIdle, reinterpret_cast<VkDevice>(2));
  });
  bool entered = false;
  {
    std::unique_lock<std::mutex> lock(state_mutex);
    entered = state_changed.wait_for(lock, std::chrono::seconds(1),
                                     [] { return waiting; });
  }
  const auto semaphore = reinterpret_cast<VkSemaphore>(32);
  VkSubmitInfo signal{};
  signal.signalSemaphoreCount = 1;
  signal.pSignalSemaphores = &semaphore;
  std::thread submitter([&] {
    EXPECT_EQ(mocktail_vulkan_submit_synchronized(
                  Submit, reinterpret_cast<VkDevice>(2),
                  reinterpret_cast<VkQueue>(30), 1, &signal, VK_NULL_HANDLE),
              VK_SUCCESS);
  });
  {
    std::unique_lock<std::mutex> lock(state_mutex);
    EXPECT_FALSE(state_changed.wait_for(lock, std::chrono::milliseconds(50),
                                        [] { return signal_submitted; }));
    idle_release = true;
    state_changed.notify_all();
  }
  idler.join();
  submitter.join();
  EXPECT_TRUE(entered);
  EXPECT_EQ(idle_result, VK_SUCCESS);
  EXPECT_TRUE(signal_submitted);
}
TEST(VrQueueSubmission, OtherDeviceCanSignalWhileDeviceIdleIsPending) {
  waiting = signal_submitted = idle_release = false;
  VkResult idle_result = VK_NOT_READY;
  // Wait emulates device A waiting for an imported semaphore signal from B.
  auto idle = [](VkDevice) -> VkResult {
    return Wait(VK_NULL_HANDLE, 0, nullptr, VK_TRUE, UINT64_MAX);
  };
  std::thread idler([&] {
    idle_result =
        mocktail_vulkan_idle_synchronized(idle, reinterpret_cast<VkDevice>(2));
  });
  bool entered = false;
  {
    std::unique_lock<std::mutex> lock(state_mutex);
    entered = state_changed.wait_for(lock, std::chrono::seconds(1),
                                     [] { return waiting; });
  }
  const auto semaphore = reinterpret_cast<VkSemaphore>(32);
  VkSubmitInfo signal{};
  signal.signalSemaphoreCount = 1;
  signal.pSignalSemaphores = &semaphore;
  EXPECT_EQ(mocktail_vulkan_submit_synchronized(
                Submit, reinterpret_cast<VkDevice>(3),
                reinterpret_cast<VkQueue>(31), 1, &signal, VK_NULL_HANDLE),
            VK_SUCCESS);
  idler.join();
  EXPECT_TRUE(entered);
  EXPECT_EQ(idle_result, VK_SUCCESS);
}
TEST(VrQueueSubmission, AnotherQueueCanSignalWhilePresentWaitsForFence) {
  waiting = signal_submitted = false;
  mocktail_vr_xr_queue_submit_adapter(mocktail_vulkan_submit_synchronized);
  const auto graphics = reinterpret_cast<VkQueue>(30);
  const auto signal_queue = reinterpret_cast<VkQueue>(31);
  const auto semaphore = reinterpret_cast<VkSemaphore>(32);
  VkPresentInfoKHR present{};
  present.waitSemaphoreCount = 1;
  present.pWaitSemaphores = &semaphore;
  VkResult prepared = VK_NOT_READY;
  std::thread presenter([&] {
    prepared = mocktail::vr::internal::WaitForPresentDependencies(
        reinterpret_cast<VkDevice>(2), graphics, present,
        reinterpret_cast<VkFence>(33), Reset, Submit, Wait);
  });
  bool entered = false;
  {
    std::unique_lock<std::mutex> lock(state_mutex);
    entered = state_changed.wait_for(lock, std::chrono::seconds(1),
                                     [] { return waiting; });
  }
  VkSubmitInfo signal{};
  signal.signalSemaphoreCount = 1;
  signal.pSignalSemaphores = &semaphore;
  EXPECT_EQ(mocktail_vulkan_submit_synchronized(
                Submit, reinterpret_cast<VkDevice>(2), signal_queue, 1, &signal,
                VK_NULL_HANDLE),
            VK_SUCCESS);
  presenter.join();
  mocktail_vr_xr_queue_submit_adapter(nullptr);
  EXPECT_TRUE(entered);
  EXPECT_EQ(prepared, VK_SUCCESS);
}
TEST(VrQueueSubmission, PrivateAndGuestCallsCannotEnterOneQueueConcurrently) {
  inside_driver = max_inside = 0;
  const auto queue = reinterpret_cast<VkQueue>(30);
  auto call = [&] {
    EXPECT_EQ(mocktail_vulkan_submit_synchronized(
                  SlowSubmit, reinterpret_cast<VkDevice>(2), queue, 0, nullptr,
                  VK_NULL_HANDLE),
              VK_SUCCESS);
  };
  std::thread first(call), second(call);
  first.join();
  second.join();
  EXPECT_EQ(max_inside, 1);
}
TEST(VrQueueSubmission, FenceWaitDoesNotKeepItsOwnQueueHostLocked) {
  waiting = signal_submitted = false;
  mocktail_vr_xr_queue_submit_adapter(mocktail_vulkan_submit_synchronized);
  const auto queue = reinterpret_cast<VkQueue>(30);
  const auto semaphore = reinterpret_cast<VkSemaphore>(32);
  VkPresentInfoKHR present{};
  present.waitSemaphoreCount = 1;
  present.pWaitSemaphores = &semaphore;
  VkResult result = VK_NOT_READY;
  std::thread presenter([&] {
    result = mocktail::vr::internal::WaitForPresentDependencies(
        reinterpret_cast<VkDevice>(2), queue, present,
        reinterpret_cast<VkFence>(33), Reset, Submit, Wait);
  });
  {
    std::unique_lock<std::mutex> lock(state_mutex);
    state_changed.wait_for(lock, std::chrono::seconds(1),
                           [] { return waiting; });
  }
  // An unrelated already-ready batch can still be enqueued to this queue.
  EXPECT_EQ(
      mocktail_vulkan_submit_synchronized(Submit, reinterpret_cast<VkDevice>(2),
                                          queue, 0, nullptr, VK_NULL_HANDLE),
      VK_SUCCESS);
  {
    std::lock_guard<std::mutex> lock(state_mutex);
    signal_submitted = true;
    state_changed.notify_all();
  }
  presenter.join();
  mocktail_vr_xr_queue_submit_adapter(nullptr);
  EXPECT_EQ(result, VK_SUCCESS);
}
} // namespace
