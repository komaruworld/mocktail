#include "runtime/process_launch_policy.h"

#include <gtest/gtest.h>
#include <sched.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <string>
#include <thread>

namespace mocktail::runtime {
namespace {

TEST(ProcessLaunchPolicyTest, SurvivesInheritedSoftCpuLimit) {
  rlimit limit{};
  ASSERT_EQ(getrlimit(RLIMIT_CPU, &limit), 0);
  if (limit.rlim_max != RLIM_INFINITY)
    GTEST_SKIP() << "Test requires an unlimited CPU hard limit";
  ASSERT_EXIT(
      {
        alarm(15);
        rlimit limit{};
        limit.rlim_cur = 1;
        limit.rlim_max = RLIM_INFINITY;
        if (setrlimit(RLIMIT_CPU, &limit) != 0) std::_Exit(10);
        const std::string diagnostics = ApplyInteractiveProcessLaunchPolicy();
        rlimit actual{};
        if (getrlimit(RLIMIT_CPU, &actual) != 0 ||
            actual.rlim_cur != RLIM_INFINITY || actual.rlim_max != RLIM_INFINITY)
          std::_Exit(11);
        // Exceed the original CPU budget. Without the startup policy the
        // kernel terminates this child with SIGXCPU rather than exit status 0.
        timespec used{};
        do {
          if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &used) != 0)
            std::_Exit(12);
        } while (used.tv_sec < 2);
        std::_Exit(diagnostics.find("cleared inherited soft CPU-time limit") !=
                          std::string::npos
                      ? 0 : 13);
      },
      ::testing::ExitedWithCode(0), "");
}

TEST(ProcessLaunchPolicyTest, PreservesFiniteHardAndSoftLimits) {
  ASSERT_EXIT(
      {
        rlimit limit{};
        limit.rlim_cur = 30;
        limit.rlim_max = 60;
        if (setrlimit(RLIMIT_CPU, &limit) != 0) std::_Exit(10);
        const std::string diagnostics = ApplyInteractiveProcessLaunchPolicy();
        rlimit actual{};
        if (getrlimit(RLIMIT_CPU, &actual) != 0 || actual.rlim_cur != 30 ||
            actual.rlim_max != 60) std::_Exit(11);
        std::_Exit(diagnostics.find("finite CPU-time limit retained") !=
                          std::string::npos
                      ? 0 : 12);
      },
      ::testing::ExitedWithCode(0), "");
}

TEST(ProcessLaunchPolicyTest, PreservesRealtimeWatchdogAndSignalHandler) {
  ASSERT_EXIT(
      {
        rlimit limit{};
        limit.rlim_cur = 100000;
        limit.rlim_max = 200000;
        if (setrlimit(RLIMIT_RTTIME, &limit) != 0) std::_Exit(10);
        if (signal(SIGXCPU, SIG_DFL) == SIG_ERR) std::_Exit(11);
        const std::string diagnostics = ApplyInteractiveProcessLaunchPolicy();
        rlimit actual{};
        struct sigaction action{};
        if (getrlimit(RLIMIT_RTTIME, &actual) != 0 ||
            actual.rlim_cur != limit.rlim_cur ||
            actual.rlim_max != limit.rlim_max ||
            sigaction(SIGXCPU, nullptr, &action) != 0 ||
            action.sa_handler != SIG_DFL) std::_Exit(12);
        std::_Exit(diagnostics.find("RLIMIT_RTTIME soft=100000 hard=200000") !=
                          std::string::npos
                      ? 0 : 13);
      },
      ::testing::ExitedWithCode(0), "");
}

TEST(ProcessLaunchPolicyTest, PreservesNonRealtimeScheduling) {
  ASSERT_EXIT(
      {
        const sched_param parameters{};
        if (sched_setscheduler(0, SCHED_BATCH, &parameters) != 0) std::_Exit(10);
        (void)ApplyInteractiveProcessLaunchPolicy();
        std::_Exit(sched_getscheduler(0) == SCHED_BATCH ? 0 : 11);
      },
      ::testing::ExitedWithCode(0), "");
}

TEST(ProcessLaunchPolicyTest, DemotesRealtimeSchedulingBeforeCreatingWorkers) {
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    alarm(5);
    sched_param parameters{};
    parameters.sched_priority = sched_get_priority_min(SCHED_FIFO);
    if (sched_setscheduler(0, SCHED_FIFO, &parameters) != 0)
      std::_Exit(errno == EPERM ? 77 : 10);
    (void)ApplyInteractiveProcessLaunchPolicy();
    if (sched_getscheduler(0) != SCHED_OTHER) std::_Exit(11);
    int worker_policy = -1;
    std::thread worker([&] { worker_policy = sched_getscheduler(0); });
    worker.join();
    std::_Exit(worker_policy == SCHED_OTHER ? 0 : 12);
  }
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  if (WEXITSTATUS(status) == 77)
    GTEST_SKIP() << "Real-time scheduling requires CAP_SYS_NICE or RLIMIT_RTPRIO";
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

}  // namespace
}  // namespace mocktail::runtime
