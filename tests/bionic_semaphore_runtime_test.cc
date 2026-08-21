#include "compat/bionic_semaphore_runtime.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <thread>

namespace {

TEST(BionicSemaphoreRuntimeTest, AdaptsGuestStorageToHostSemaphore) {
  alignas(sem_t) std::array<unsigned char, sizeof(sem_t)> guest_storage{};
  auto* semaphore = reinterpret_cast<sem_t*>(guest_storage.data());

  ASSERT_EQ(mocktail_bionic_sem_init(semaphore, 0, 0), 0);
  std::atomic<bool> released{false};
  std::thread waiter([&] {
    EXPECT_EQ(mocktail_bionic_sem_wait(semaphore), 0);
    released.store(true, std::memory_order_release);
  });

  EXPECT_FALSE(released.load(std::memory_order_acquire));
  EXPECT_EQ(mocktail_bionic_sem_post(semaphore), 0);
  waiter.join();
  EXPECT_TRUE(released.load(std::memory_order_acquire));
  EXPECT_EQ(mocktail_bionic_sem_destroy(semaphore), 0);
}

TEST(BionicSemaphoreRuntimeTest, RejectsProcessSharedSemaphore) {
  sem_t guest_storage{};
  errno = 0;
  EXPECT_EQ(mocktail_bionic_sem_init(&guest_storage, 1, 0), -1);
  EXPECT_EQ(errno, EINVAL);
}

}  // namespace
