#include "runtime/memory_limit.h"

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>

namespace mocktail {
namespace runtime {
namespace {

TEST(MemoryLimitTest, ParsesResidentAndSwapMemoryFromProcStatus) {
  const ProcessMemoryUsage usage = ParseProcessStatusMemory(R"(
Name:	mocktail
VmSize:	999999 kB
VmRSS:	12345 kB
VmSwap:	678 kB
Threads:	42
)");

  ASSERT_TRUE(usage.valid);
  EXPECT_EQ(usage.resident_bytes, 12345U * 1024U);
  EXPECT_EQ(usage.swap_bytes, 678U * 1024U);
  EXPECT_EQ(usage.committed_bytes(), (12345U + 678U) * 1024U);
}

TEST(MemoryLimitTest, AcceptsKernelStatusWithoutSwapField) {
  const ProcessMemoryUsage usage =
      ParseProcessStatusMemory("Name:\tmocktail\nVmRSS:\t2048 kB\n");

  ASSERT_TRUE(usage.valid);
  EXPECT_EQ(usage.resident_bytes, 2U * 1024U * 1024U);
  EXPECT_EQ(usage.swap_bytes, 0U);
}

TEST(MemoryLimitTest, RejectsMissingOrMalformedResidentMemory) {
  EXPECT_FALSE(ParseProcessStatusMemory("VmSwap:\t12 kB\n").valid);
  EXPECT_FALSE(ParseProcessStatusMemory("VmRSS:\t12 MB\n").valid);
  EXPECT_FALSE(ParseProcessStatusMemory("VmRSS:\tnot-a-number kB\n").valid);
}

TEST(MemoryLimitTest, StartsAndPhysicallyJoinsWatchdog) {
  MemoryLimitWatchdog watchdog;
  std::string error;

  ASSERT_TRUE(watchdog.Start(std::numeric_limits<std::uint64_t>::max(),
                             &error))
      << error;
  EXPECT_TRUE(watchdog.running());
  watchdog.Stop();
  EXPECT_FALSE(watchdog.running());
}

TEST(MemoryLimitTest, RejectsDisabledOrDuplicateWatchdogStart) {
  MemoryLimitWatchdog watchdog;
  std::string error;
  EXPECT_FALSE(watchdog.Start(0, &error));
  EXPECT_NE(error.find("non-zero"), std::string::npos);

  error.clear();
  ASSERT_TRUE(watchdog.Start(std::numeric_limits<std::uint64_t>::max(),
                             &error))
      << error;
  EXPECT_FALSE(watchdog.Start(1024U * 1024U, &error));
  EXPECT_NE(error.find("already running"), std::string::npos);
  watchdog.Stop();
}

TEST(MemoryLimitTest, ReexecutesInsideVerifiedCgroupV2LimitWhenAvailable) {
  constexpr std::uint64_t kTestLimitBytes = 64ULL * 1024ULL * 1024ULL;
  constexpr char kChildMarker[] = "MOCKTAIL_MEMORY_LIMIT_TEST_CHILD";
  char program[] = "memory_limit_test";
  char filter[] =
      "--gtest_filter=MemoryLimitTest."
      "ReexecutesInsideVerifiedCgroupV2LimitWhenAvailable";
  char* arguments[] = {program, filter, nullptr};

  if (getenv(kChildMarker) == nullptr) {
    ASSERT_EQ(setenv(kChildMarker, "1", 1), 0);
  }
  const CgroupMemoryLimitResult result =
      MaybeReexecWithCgroupMemoryLimit(2, arguments, kTestLimitBytes);
  if (result.status == CgroupMemoryLimitStatus::kUnavailable) {
    (void)unsetenv(kChildMarker);
    GTEST_SKIP() << result.detail;
  }
  ASSERT_EQ(result.status, CgroupMemoryLimitStatus::kActive) << result.detail;
  EXPECT_TRUE(result.active());
  (void)unsetenv(kChildMarker);
}

TEST(MemoryLimitTest, TerminatesOnlyGuardedProcessWithExit137) {
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    MemoryLimitWatchdog watchdog;
    std::string error;
    if (!watchdog.Start(1, &error)) {
      std::_Exit(2);
    }
    while (true) {
      pause();
    }
  }

  int child_status = 0;
  pid_t waited = -1;
  do {
    waited = waitpid(child, &child_status, 0);
  } while (waited < 0 && errno == EINTR);
  ASSERT_EQ(waited, child);
  ASSERT_TRUE(WIFEXITED(child_status));
  EXPECT_EQ(WEXITSTATUS(child_status), 137);
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
