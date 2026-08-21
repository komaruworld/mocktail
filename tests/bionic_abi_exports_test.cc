#include "compat/bionic_abi_exports.h"

#include <gtest/gtest.h>

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cstdarg>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {

int CheckedFormat(char* destination, size_t count, size_t destination_size,
                  const char* format, ...) {
  va_list arguments;
  va_start(arguments, format);
  const int result = __vsnprintf_chk(destination, count, 0, destination_size,
                                     format, arguments);
  va_end(arguments);
  return result;
}

TEST(BionicAbiExportsTest, ReportsStableAndroidSystemProperties) {
  std::array<char, 92> value{};

  EXPECT_EQ(2, __system_property_get("ro.build.version.sdk", value.data()));
  EXPECT_STREQ("35", value.data());

  value.fill('\0');
  EXPECT_EQ(6, __system_property_get("ro.product.cpu.abi", value.data()));
  EXPECT_STREQ("x86_64", value.data());

  value.fill('x');
  EXPECT_EQ(0, __system_property_get("mocktail.unknown", value.data()));
  EXPECT_STREQ("", value.data());
  EXPECT_EQ(0, __system_property_get("ro.build.version.sdk", nullptr));
}

TEST(BionicAbiExportsTest, ReportsConfiguredMarketingDeviceIdentity) {
  ASSERT_EQ(0, setenv("MOCKTAIL_DEVICE_MANUFACTURER", "Google", 1));
  ASSERT_EQ(0, setenv("MOCKTAIL_DEVICE_MODEL", "Pixel 7", 1));
  ASSERT_EQ(0, setenv("MOCKTAIL_DEVICE_BRAND", "google", 1));
  ASSERT_EQ(0, setenv("MOCKTAIL_DEVICE_CODE", "panther", 1));
  std::array<char, 92> value{};

  EXPECT_GT(__system_property_get("ro.product.manufacturer", value.data()), 0);
  EXPECT_STREQ("Google", value.data());
  EXPECT_GT(__system_property_get("ro.product.model", value.data()), 0);
  EXPECT_STREQ("Pixel 7", value.data());
  EXPECT_GT(__system_property_get("ro.product.brand", value.data()), 0);
  EXPECT_STREQ("google", value.data());
  EXPECT_GT(__system_property_get("ro.product.device", value.data()), 0);
  EXPECT_STREQ("panther", value.data());

  unsetenv("MOCKTAIL_DEVICE_MANUFACTURER");
  unsetenv("MOCKTAIL_DEVICE_MODEL");
  unsetenv("MOCKTAIL_DEVICE_BRAND");
  unsetenv("MOCKTAIL_DEVICE_CODE");
}

TEST(BionicAbiExportsTest, CheckedMemcpyClampsToDestination) {
  using CheckedMemcpy = void* (*)(void*, const void*, size_t, size_t);
  volatile CheckedMemcpy checked_memcpy = &__memcpy_chk;
  const std::array<char, 6> source = {'a', 'b', 'c', 'd', 'e', 'f'};
  std::array<char, 4> destination = {'x', 'x', 'x', 'x'};

  errno = 0;
  EXPECT_EQ(destination.data(),
            checked_memcpy(destination.data(), source.data(), source.size(),
                           destination.size()));
  EXPECT_EQ(ERANGE, errno);
  EXPECT_EQ((std::array<char, 4>{'a', 'b', 'c', 'd'}), destination);
}

TEST(BionicAbiExportsTest, CheckedStrcpyClampsAndTerminates) {
  using CheckedStrcpy = char* (*)(char*, const char*, size_t);
  volatile CheckedStrcpy checked_strcpy = &__strcpy_chk;
  std::array<char, 4> destination{};

  errno = 0;
  EXPECT_EQ(destination.data(),
            checked_strcpy(destination.data(), "abcdef", destination.size()));
  EXPECT_EQ(ERANGE, errno);
  EXPECT_STREQ("abc", destination.data());
}

TEST(BionicAbiExportsTest, CheckedVsnprintfUsesHostFormatterAndClampsOutput) {
  std::array<char, 8> destination{};

  EXPECT_EQ(11, CheckedFormat(destination.data(), 64, destination.size(),
                              "%s:%d", "mocktail", 42));
  EXPECT_STREQ("mocktai", destination.data());
}

TEST(BionicAbiExportsTest, CheckedReadCallsHostLibcAndClampsInput) {
  using CheckedRead = ssize_t (*)(int, void*, size_t, size_t);
  volatile CheckedRead checked_read = &__read_chk;
  int pipe_fds[2] = {-1, -1};
  ASSERT_EQ(0, pipe(pipe_fds));
  ASSERT_EQ(6, write(pipe_fds[1], "abcdef", 6));
  ASSERT_EQ(0, close(pipe_fds[1]));

  std::array<char, 4> destination{};
  errno = 0;
  EXPECT_EQ(4, checked_read(pipe_fds[0], destination.data(), 6,
                            destination.size()));
  EXPECT_EQ(ERANGE, errno);
  EXPECT_EQ((std::array<char, 4>{'a', 'b', 'c', 'd'}), destination);
  EXPECT_EQ(0, close(pipe_fds[0]));
}

TEST(BionicAbiExportsTest, PublishedJniEnvironmentRoundTrips) {
  int marker = 0;
  mocktail_set_current_jni_env(&marker);
  EXPECT_EQ(&marker, mocktail_get_current_jni_env());

  mocktail_set_current_jni_env(nullptr);
  EXPECT_EQ(nullptr, mocktail_get_current_jni_env());
}

TEST(BionicAbiExportsTest, AdaptsManyIndependentGuestMutexAddresses) {
  std::array<pthread_mutex_t, 4096> guest_mutexes{};

  for (pthread_mutex_t& guest_mutex : guest_mutexes) {
    ASSERT_EQ(0, mocktail_pthread_mutex_init(&guest_mutex, nullptr));
    ASSERT_EQ(0, mocktail_pthread_mutex_lock(&guest_mutex));
    ASSERT_EQ(0, mocktail_pthread_mutex_unlock(&guest_mutex));
  }
  for (auto iterator = guest_mutexes.rbegin(); iterator != guest_mutexes.rend();
       ++iterator) {
    EXPECT_EQ(0, mocktail_pthread_mutex_destroy(&*iterator));
  }
}

TEST(BionicAbiExportsTest, SerializesConcurrentAccessToOneGuestMutex) {
  pthread_mutex_t guest_mutex{};
  ASSERT_EQ(0, mocktail_pthread_mutex_init(&guest_mutex, nullptr));
  std::atomic<int> counter{0};
  std::atomic<int> failures{0};
  std::vector<std::thread> workers;
  for (int worker = 0; worker < 8; ++worker) {
    workers.emplace_back([&] {
      for (int iteration = 0; iteration < 2000; ++iteration) {
        if (mocktail_pthread_mutex_lock(&guest_mutex) != 0) {
          ++failures;
          continue;
        }
        counter.store(counter.load(std::memory_order_relaxed) + 1,
                      std::memory_order_relaxed);
        if (mocktail_pthread_mutex_unlock(&guest_mutex) != 0) {
          ++failures;
        }
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }

  EXPECT_EQ(0, failures.load());
  EXPECT_EQ(16000, counter.load());
  EXPECT_EQ(0, mocktail_pthread_mutex_destroy(&guest_mutex));
  EXPECT_EQ(0, mocktail_pthread_mutex_init(&guest_mutex, nullptr));
  EXPECT_EQ(0, mocktail_pthread_mutex_lock(&guest_mutex));
  EXPECT_EQ(0, mocktail_pthread_mutex_unlock(&guest_mutex));
  EXPECT_EQ(0, mocktail_pthread_mutex_destroy(&guest_mutex));
}

TEST(BionicAbiExportsTest, ReleasesDefaultMutexFromAnotherThreadLikeBionic) {
  pthread_mutex_t guest_mutex{};
  ASSERT_EQ(0, mocktail_pthread_mutex_init(&guest_mutex, nullptr));
  ASSERT_EQ(0, mocktail_pthread_mutex_lock(&guest_mutex));

  std::atomic<int> unlock_result{EINVAL};
  std::atomic<int> relock_result{EINVAL};
  std::thread worker([&] {
    unlock_result.store(mocktail_pthread_mutex_unlock(&guest_mutex),
                        std::memory_order_release);
    const int result = mocktail_pthread_mutex_trylock(&guest_mutex);
    relock_result.store(result, std::memory_order_release);
    if (result == 0) {
      mocktail_pthread_mutex_unlock(&guest_mutex);
    }
  });
  worker.join();

  EXPECT_EQ(0, unlock_result.load(std::memory_order_acquire));
  const int observed_relock = relock_result.load(std::memory_order_acquire);
  EXPECT_EQ(0, observed_relock);
  if (observed_relock != 0) {
    EXPECT_EQ(0, mocktail_pthread_mutex_unlock(&guest_mutex));
  }
  EXPECT_EQ(0, mocktail_pthread_mutex_destroy(&guest_mutex));
}

TEST(BionicAbiExportsTest, WakesBlockedWaiterAfterForeignDefaultUnlock) {
  pthread_mutex_t guest_mutex{};
  ASSERT_EQ(0, mocktail_pthread_mutex_init(&guest_mutex, nullptr));
  ASSERT_EQ(0, mocktail_pthread_mutex_lock(&guest_mutex));

  std::atomic<bool> waiter_started{false};
  std::atomic<bool> waiter_acquired{false};
  std::atomic<int> waiter_lock_result{EINVAL};
  std::thread waiter([&] {
    waiter_started.store(true, std::memory_order_release);
    const int result = mocktail_pthread_mutex_lock(&guest_mutex);
    waiter_lock_result.store(result, std::memory_order_release);
    if (result == 0) {
      waiter_acquired.store(true, std::memory_order_release);
      mocktail_pthread_mutex_unlock(&guest_mutex);
    }
  });
  while (!waiter_started.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  std::atomic<int> foreign_unlock_result{EINVAL};
  std::thread releaser([&] {
    foreign_unlock_result.store(mocktail_pthread_mutex_unlock(&guest_mutex),
                                std::memory_order_release);
  });
  releaser.join();

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
  while (!waiter_acquired.load(std::memory_order_acquire) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  const bool required_owner_cleanup =
      !waiter_acquired.load(std::memory_order_acquire);
  if (required_owner_cleanup) {
    mocktail_pthread_mutex_unlock(&guest_mutex);
  }
  waiter.join();

  EXPECT_EQ(0, foreign_unlock_result.load(std::memory_order_acquire));
  EXPECT_FALSE(required_owner_cleanup);
  EXPECT_EQ(0, waiter_lock_result.load(std::memory_order_acquire));
  EXPECT_EQ(0, mocktail_pthread_mutex_destroy(&guest_mutex));
}

TEST(BionicAbiExportsTest, PreservesRecursiveMutexAttributesAndOwnership) {
  MocktailBionicMutexAttr guest_attributes = 0;
  ASSERT_EQ(0, mocktail_pthread_mutexattr_init(&guest_attributes));
  EXPECT_EQ(EINVAL, mocktail_pthread_mutexattr_settype(&guest_attributes, -1));
  EXPECT_EQ(EINVAL, mocktail_pthread_mutexattr_settype(&guest_attributes, 3));
  ASSERT_EQ(0, mocktail_pthread_mutexattr_settype(&guest_attributes, 1));

  pthread_mutex_t guest_mutex{};
  ASSERT_EQ(0, mocktail_pthread_mutex_init(&guest_mutex, &guest_attributes));
  ASSERT_EQ(0, mocktail_pthread_mutex_lock(&guest_mutex));
  ASSERT_EQ(0, mocktail_pthread_mutex_trylock(&guest_mutex));

  std::atomic<int> foreign_unlock_result{0};
  std::thread worker([&] {
    foreign_unlock_result.store(mocktail_pthread_mutex_unlock(&guest_mutex),
                                std::memory_order_release);
  });
  worker.join();

  EXPECT_EQ(EPERM, foreign_unlock_result.load(std::memory_order_acquire));
  EXPECT_EQ(0, mocktail_pthread_mutex_unlock(&guest_mutex));
  EXPECT_EQ(0, mocktail_pthread_mutex_unlock(&guest_mutex));
  EXPECT_EQ(0, mocktail_pthread_mutex_destroy(&guest_mutex));
  EXPECT_EQ(0, mocktail_pthread_mutexattr_destroy(&guest_attributes));
  EXPECT_EQ(-1, guest_attributes);
}

TEST(BionicAbiExportsTest, RejectsDestroyOfLockedMutex) {
  pthread_mutex_t guest_mutex{};
  ASSERT_EQ(0, mocktail_pthread_mutex_init(&guest_mutex, nullptr));
  ASSERT_EQ(0, mocktail_pthread_mutex_lock(&guest_mutex));

  EXPECT_EQ(EBUSY, mocktail_pthread_mutex_destroy(&guest_mutex));
  EXPECT_EQ(0, mocktail_pthread_mutex_unlock(&guest_mutex));
  EXPECT_EQ(0, mocktail_pthread_mutex_destroy(&guest_mutex));
}

TEST(BionicAbiExportsTest, RejectsDestroyOfRecursivelyLockedMutex) {
  MocktailBionicMutexAttr guest_attributes = 0;
  ASSERT_EQ(0, mocktail_pthread_mutexattr_init(&guest_attributes));
  ASSERT_EQ(0, mocktail_pthread_mutexattr_settype(&guest_attributes, 1));

  pthread_mutex_t guest_mutex{};
  ASSERT_EQ(0, mocktail_pthread_mutex_init(&guest_mutex, &guest_attributes));
  ASSERT_EQ(0, mocktail_pthread_mutex_lock(&guest_mutex));
  ASSERT_EQ(0, mocktail_pthread_mutex_lock(&guest_mutex));

  EXPECT_EQ(EBUSY, mocktail_pthread_mutex_destroy(&guest_mutex));
  EXPECT_EQ(0, mocktail_pthread_mutex_unlock(&guest_mutex));
  EXPECT_EQ(EBUSY, mocktail_pthread_mutex_destroy(&guest_mutex));
  EXPECT_EQ(0, mocktail_pthread_mutex_unlock(&guest_mutex));
  EXPECT_EQ(0, mocktail_pthread_mutex_destroy(&guest_mutex));
  EXPECT_EQ(0, mocktail_pthread_mutexattr_destroy(&guest_attributes));
}

TEST(BionicAbiExportsTest, PreservesMonotonicConditionClockForTimedWait) {
  pthread_condattr_t guest_attributes{};
  ASSERT_EQ(0, mocktail_pthread_condattr_init(&guest_attributes));
  ASSERT_EQ(0, mocktail_pthread_condattr_setclock(&guest_attributes,
                                                  CLOCK_MONOTONIC));

  pthread_cond_t guest_condition{};
  pthread_mutex_t guest_mutex{};
  ASSERT_EQ(0, mocktail_pthread_cond_init(&guest_condition,
                                          &guest_attributes));
  ASSERT_EQ(0, mocktail_pthread_condattr_destroy(&guest_attributes));
  ASSERT_EQ(0, mocktail_pthread_mutex_init(&guest_mutex, nullptr));
  ASSERT_EQ(0, mocktail_pthread_mutex_lock(&guest_mutex));

  std::thread signaler([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_EQ(0, mocktail_pthread_mutex_lock(&guest_mutex));
    EXPECT_EQ(0, mocktail_pthread_cond_signal(&guest_condition));
    EXPECT_EQ(0, mocktail_pthread_mutex_unlock(&guest_mutex));
  });

  timespec deadline{};
  ASSERT_EQ(0, clock_gettime(CLOCK_MONOTONIC, &deadline));
  deadline.tv_sec += 2;
  const auto started = std::chrono::steady_clock::now();
  EXPECT_EQ(0, mocktail_pthread_cond_timedwait(
                   &guest_condition, &guest_mutex, &deadline));
  const auto elapsed = std::chrono::steady_clock::now() - started;
  EXPECT_GE(elapsed, std::chrono::milliseconds(10));
  EXPECT_LT(elapsed, std::chrono::seconds(1));

  EXPECT_EQ(0, mocktail_pthread_mutex_unlock(&guest_mutex));
  signaler.join();
  EXPECT_EQ(0, mocktail_pthread_cond_destroy(&guest_condition));
  EXPECT_EQ(0, mocktail_pthread_mutex_destroy(&guest_mutex));
}

TEST(BionicAbiExportsTest, InvalidatesRemoteMutexCacheAcrossAddressReuse) {
  pthread_mutex_t guest_mutex{};
  pthread_mutex_t allocator_spacer{};
  ASSERT_EQ(0, mocktail_pthread_mutex_init(&guest_mutex, nullptr));

  std::atomic<bool> cache_ready{false};
  std::atomic<bool> replacement_locked{false};
  std::atomic<int> observed_trylock{0};
  std::thread worker([&] {
    ASSERT_EQ(0, mocktail_pthread_mutex_lock(&guest_mutex));
    ASSERT_EQ(0, mocktail_pthread_mutex_unlock(&guest_mutex));
    cache_ready.store(true, std::memory_order_release);
    while (!replacement_locked.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    const int result = mocktail_pthread_mutex_trylock(&guest_mutex);
    observed_trylock.store(result, std::memory_order_release);
    if (result == 0) {
      EXPECT_EQ(0, mocktail_pthread_mutex_unlock(&guest_mutex));
    }
  });

  while (!cache_ready.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  ASSERT_EQ(0, mocktail_pthread_mutex_destroy(&guest_mutex));
  ASSERT_EQ(0, mocktail_pthread_mutex_init(&allocator_spacer, nullptr));
  ASSERT_EQ(0, mocktail_pthread_mutex_init(&guest_mutex, nullptr));
  ASSERT_EQ(0, mocktail_pthread_mutex_lock(&guest_mutex));
  replacement_locked.store(true, std::memory_order_release);
  worker.join();

  EXPECT_EQ(EBUSY, observed_trylock.load(std::memory_order_acquire));
  EXPECT_EQ(0, mocktail_pthread_mutex_unlock(&guest_mutex));
  EXPECT_EQ(0, mocktail_pthread_mutex_destroy(&guest_mutex));
  EXPECT_EQ(0, mocktail_pthread_mutex_destroy(&allocator_spacer));
}

}  // namespace
