#include "compat/bionic_rwlock_runtime.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>

namespace {

TEST(BionicRwlockRuntimeTest, SerializesGuestReadersAndWriter) {
  pthread_rwlock_t guest_storage{};
  ASSERT_EQ(mocktail_bionic_pthread_rwlock_init(&guest_storage, nullptr), 0);
  ASSERT_EQ(mocktail_bionic_pthread_rwlock_rdlock(&guest_storage), 0);

  std::atomic<bool> writer_entered{false};
  std::thread writer([&] {
    EXPECT_EQ(mocktail_bionic_pthread_rwlock_wrlock(&guest_storage), 0);
    writer_entered.store(true, std::memory_order_release);
    EXPECT_EQ(mocktail_bionic_pthread_rwlock_unlock(&guest_storage), 0);
  });

  EXPECT_FALSE(writer_entered.load(std::memory_order_acquire));
  EXPECT_EQ(mocktail_bionic_pthread_rwlock_unlock(&guest_storage), 0);
  writer.join();
  EXPECT_TRUE(writer_entered.load(std::memory_order_acquire));
  EXPECT_EQ(mocktail_bionic_pthread_rwlock_destroy(&guest_storage), 0);
}

TEST(BionicRwlockRuntimeTest, SupportsZeroInitializedGuestLock) {
  pthread_rwlock_t guest_storage{};
  EXPECT_EQ(mocktail_bionic_pthread_rwlock_wrlock(&guest_storage), 0);
  EXPECT_EQ(mocktail_bionic_pthread_rwlock_unlock(&guest_storage), 0);
  EXPECT_EQ(mocktail_bionic_pthread_rwlock_destroy(&guest_storage), 0);
}

}  // namespace
