#include "compat/http_client_spin_guard.h"

#include <gtest/gtest.h>

#include <chrono>
#include <thread>

namespace mocktail {
namespace compat {
namespace {

TEST(HttpClientSpinGuardTest, LeavesFirstAndSlowBatchesUnthrottled) {
  HttpClientSpinRateLimiter limiter;
  for (std::uint32_t operation = 1;
       operation < kHttpClientSpinBatchSize; ++operation) {
    EXPECT_EQ(limiter.Observe(100), 0U);
  }
  EXPECT_EQ(limiter.Observe(100), 0U);

  for (std::uint32_t operation = 1;
       operation < kHttpClientSpinBatchSize; ++operation) {
    EXPECT_EQ(limiter.Observe(2000000), 0U);
  }
  EXPECT_EQ(limiter.Observe(2000000), 0U);
}

TEST(HttpClientSpinGuardTest, DelaysOnlyTheRemainderOfAFastBatch) {
  HttpClientSpinRateLimiter limiter;
  for (std::uint32_t operation = 0;
       operation < kHttpClientSpinBatchSize; ++operation) {
    EXPECT_EQ(limiter.Observe(1000000), 0U);
  }
  for (std::uint32_t operation = 1;
       operation < kHttpClientSpinBatchSize; ++operation) {
    EXPECT_EQ(limiter.Observe(1250000), 0U);
  }
  EXPECT_EQ(limiter.Observe(1250000), 750000U);
}

TEST(HttpClientSpinGuardTest, ResetsAfterClockDiscontinuity) {
  HttpClientSpinRateLimiter limiter;
  for (std::uint32_t operation = 0;
       operation < kHttpClientSpinBatchSize; ++operation) {
    EXPECT_EQ(limiter.Observe(2000000), 0U);
  }
  for (std::uint32_t operation = 0;
       operation < kHttpClientSpinBatchSize; ++operation) {
    EXPECT_EQ(limiter.Observe(1000000), 0U);
  }
}

TEST(HttpClientSpinGuardTest, ThrottlesOnlyAfterFastHttpClientBatches) {
  std::chrono::nanoseconds elapsed{};
  std::thread worker([&elapsed]() {
    ASSERT_EQ(pthread_setname_np(pthread_self(), "HttpClient"), 0);
    const auto started = std::chrono::steady_clock::now();
    for (std::uint32_t operation = 0;
         operation < kHttpClientSpinBatchSize * 2; ++operation) {
      ApplyHttpClientSpinGuard();
    }
    elapsed = std::chrono::steady_clock::now() - started;
  });
  worker.join();

  // The first batch establishes a checkpoint. The second fast batch sleeps
  // until the one-millisecond cooperative interval, with generous tolerance
  // for clock and syscall granularity.
  EXPECT_GE(elapsed, std::chrono::microseconds(500));
}

}  // namespace
}  // namespace compat
}  // namespace mocktail
