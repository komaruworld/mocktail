#include "compat/bionic_pthread_key_runtime.h"

#include <gtest/gtest.h>

#include <atomic>
#include <array>
#include <cerrno>
#include <cstdint>
#include <thread>

namespace mocktail::compat {
namespace {

std::atomic<int> g_destructor_calls{0};
pthread_key_t g_rearming_key{};

void CountDestructor(void* value) {
  if (value != nullptr) {
    ++g_destructor_calls;
  }
}

void RearmDestructor(void* value) {
  ++g_destructor_calls;
  mocktail_bionic_pthread_setspecific(g_rearming_key, value);
}

TEST(BionicPthreadKeyRuntimeTest, CreatesHighBitBionicKeyAndStoresValue) {
  pthread_key_t key{};
  ASSERT_EQ(mocktail_bionic_pthread_key_create(&key, nullptr), 0);
  EXPECT_TRUE(IsBionicPthreadKey(key));
  EXPECT_NE(static_cast<uint32_t>(key) & kBionicPthreadKeyValidFlag, 0U);

  int value = 42;
  EXPECT_EQ(mocktail_bionic_pthread_getspecific(key), nullptr);
  EXPECT_EQ(mocktail_bionic_pthread_setspecific(key, &value), 0);
  EXPECT_EQ(mocktail_bionic_pthread_getspecific(key), &value);
  EXPECT_EQ(mocktail_bionic_pthread_key_delete(key), 0);
}

TEST(BionicPthreadKeyRuntimeTest, KeepsValuesIsolatedPerThread) {
  pthread_key_t key{};
  ASSERT_EQ(mocktail_bionic_pthread_key_create(&key, nullptr), 0);

  int main_value = 1;
  int worker_value = 2;
  ASSERT_EQ(mocktail_bionic_pthread_setspecific(key, &main_value), 0);

  std::thread worker([&]() {
    EXPECT_EQ(mocktail_bionic_pthread_getspecific(key), nullptr);
    EXPECT_EQ(mocktail_bionic_pthread_setspecific(key, &worker_value), 0);
    EXPECT_EQ(mocktail_bionic_pthread_getspecific(key), &worker_value);
  });
  worker.join();

  EXPECT_EQ(mocktail_bionic_pthread_getspecific(key), &main_value);
  EXPECT_EQ(mocktail_bionic_pthread_key_delete(key), 0);
}

TEST(BionicPthreadKeyRuntimeTest, DeleteAndReuseInvalidatesStaleValue) {
  pthread_key_t first_key{};
  ASSERT_EQ(mocktail_bionic_pthread_key_create(&first_key, nullptr), 0);
  int stale_value = 7;
  ASSERT_EQ(mocktail_bionic_pthread_setspecific(first_key, &stale_value), 0);
  ASSERT_EQ(mocktail_bionic_pthread_key_delete(first_key), 0);

  pthread_key_t reused_key{};
  ASSERT_EQ(mocktail_bionic_pthread_key_create(&reused_key, nullptr), 0);
  EXPECT_EQ(mocktail_bionic_pthread_getspecific(reused_key), nullptr);
  EXPECT_EQ(mocktail_bionic_pthread_key_delete(reused_key), 0);
}

TEST(BionicPthreadKeyRuntimeTest, RunsDestructorAtThreadExit) {
  pthread_key_t key{};
  ASSERT_EQ(mocktail_bionic_pthread_key_create(&key, CountDestructor), 0);
  g_destructor_calls.store(0);

  std::thread worker([&]() {
    int value = 9;
    EXPECT_EQ(mocktail_bionic_pthread_setspecific(key, &value), 0);
  });
  worker.join();

  EXPECT_EQ(g_destructor_calls.load(), 1);
  EXPECT_EQ(mocktail_bionic_pthread_key_delete(key), 0);
}

TEST(BionicPthreadKeyRuntimeTest, SupportsFullBionicApplicationKeyCapacity) {
  constexpr size_t kApplicationKeyCount = kBionicPthreadKeyCount - 2;
  std::array<pthread_key_t, kApplicationKeyCount> keys{};
  for (pthread_key_t& key : keys) {
    ASSERT_EQ(mocktail_bionic_pthread_key_create(&key, nullptr), 0);
  }
  pthread_key_t exhausted{};
  EXPECT_EQ(mocktail_bionic_pthread_key_create(&exhausted, nullptr), EAGAIN);
  for (pthread_key_t key : keys) {
    EXPECT_EQ(mocktail_bionic_pthread_key_delete(key), 0);
  }
}

TEST(BionicPthreadKeyRuntimeTest,
     ReusedKeyInvalidatesRemoteThreadValueAndDestructor) {
  pthread_key_t old_key{};
  ASSERT_EQ(mocktail_bionic_pthread_key_create(&old_key, CountDestructor), 0);
  g_destructor_calls.store(0);
  std::atomic<bool> value_ready{false};
  std::atomic<bool> key_reused{false};
  std::atomic<void*> observed{reinterpret_cast<void*>(1)};
  int value = 17;

  std::thread worker([&] {
    EXPECT_EQ(mocktail_bionic_pthread_setspecific(old_key, &value), 0);
    value_ready.store(true, std::memory_order_release);
    while (!key_reused.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    observed.store(mocktail_bionic_pthread_getspecific(old_key),
                   std::memory_order_release);
  });
  while (!value_ready.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  ASSERT_EQ(mocktail_bionic_pthread_key_delete(old_key), 0);
  pthread_key_t reused_key{};
  ASSERT_EQ(mocktail_bionic_pthread_key_create(&reused_key, CountDestructor),
            0);
  ASSERT_EQ(reused_key, old_key);
  key_reused.store(true, std::memory_order_release);
  worker.join();

  EXPECT_EQ(observed.load(std::memory_order_acquire), nullptr);
  EXPECT_EQ(g_destructor_calls.load(), 0);
  EXPECT_EQ(mocktail_bionic_pthread_key_delete(reused_key), 0);
}

TEST(BionicPthreadKeyRuntimeTest, RunsRearmedDestructorFourTimes) {
  ASSERT_EQ(
      mocktail_bionic_pthread_key_create(&g_rearming_key, RearmDestructor), 0);
  g_destructor_calls.store(0);

  std::thread worker([] {
    static int value = 23;
    EXPECT_EQ(mocktail_bionic_pthread_setspecific(g_rearming_key, &value), 0);
  });
  worker.join();

  EXPECT_EQ(g_destructor_calls.load(), 4);
  EXPECT_EQ(mocktail_bionic_pthread_key_delete(g_rearming_key), 0);
}

}  // namespace
}  // namespace mocktail::compat
