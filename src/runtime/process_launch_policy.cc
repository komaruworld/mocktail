#include "runtime/process_launch_policy.h"

#include <sched.h>
#include <strings.h>
#include <sys/resource.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace mocktail::runtime {
namespace {

std::string LimitValue(rlim_t value) {
  return value == RLIM_INFINITY ? "unlimited" : std::to_string(value);
}

void LogLimit(std::ostringstream& log, const char* name, const rlimit& limit,
              const char* units) {
  log << "  [process] " << name << " soft=" << LimitValue(limit.rlim_cur)
      << " hard=" << LimitValue(limit.rlim_max) << " units=" << units << '\n';
}

void ClearInheritedMimallocEnvironment(std::ostringstream& log) {
  // Desktop shells and launchers that use mimalloc may export
  // MIMALLOC_ARENA_RESERVE=0 or MIMALLOC_PURGE_DELAY=0 into their own
  // environment. libroblox embeds its own mimalloc whose _mi_options_init
  // constructor scans environ case-insensitively for MIMALLOC_*; inheriting
  // MIMALLOC_ARENA_RESERVE=0 disables its 1 GiB arena reservation and forces
  // 64 KiB-aligned segment allocations onto an over-allocation fallback that
  // crashes during startup.
  std::vector<std::string> names;
  if (environ != nullptr) {
    for (char** entry = environ; *entry != nullptr; ++entry) {
      const char* equals = std::strchr(*entry, '=');
      const std::size_t length =
          equals != nullptr ? static_cast<std::size_t>(equals - *entry)
                            : std::strlen(*entry);
      if (length >= 9 && strncasecmp(*entry, "MIMALLOC_", 9) == 0) {
        names.emplace_back(*entry, length);
      }
    }
  }
  for (const std::string& name : names) {
    if (unsetenv(name.c_str()) == 0) {
      log << "  [process] cleared inherited host mimalloc variable: " << name
          << '\n';
    } else {
      log << "  [process] cannot clear inherited host mimalloc variable "
          << name << ": " << std::strerror(errno) << '\n';
    }
  }
}

}  // namespace

std::string ApplyInteractiveProcessLaunchPolicy() {
  std::ostringstream log;
  // Browser protocol handlers inherit the browser's scheduling policy. A
  // compute-heavy engine thread running FIFO/RR can exhaust RLIMIT_RTTIME and
  // receive SIGXCPU even when the ordinary CPU-time limit is unlimited.
  const int policy = sched_getscheduler(0);
  if (policy < 0) {
    log << "  [process] cannot read scheduling policy: " << std::strerror(errno)
        << '\n';
  } else {
    log << "  [process] inherited scheduler=" << policy << '\n';
    const int base_policy = policy & ~SCHED_RESET_ON_FORK;
    if (base_policy == SCHED_FIFO || base_policy == SCHED_RR) {
      const sched_param parameters{};
      // Clearing RESET_ON_FORK requires CAP_SYS_NICE, even when demoting.
      const int target_policy = SCHED_OTHER | (policy & SCHED_RESET_ON_FORK);
      if (sched_setscheduler(0, target_policy, &parameters) == 0) {
        log << "  [process] reset inherited real-time scheduler to SCHED_OTHER\n";
      } else {
        log << "  [process] cannot reset real-time scheduler: "
            << std::strerror(errno) << "; RLIMIT_RTTIME may terminate startup\n";
      }
    }
  }

  rlimit cpu{};
  if (getrlimit(RLIMIT_CPU, &cpu) == 0) {
    LogLimit(log, "RLIMIT_CPU", cpu, "seconds");
    if (cpu.rlim_cur != RLIM_INFINITY && cpu.rlim_max == RLIM_INFINITY) {
      cpu.rlim_cur = RLIM_INFINITY;
      if (setrlimit(RLIMIT_CPU, &cpu) == 0) {
        log << "  [process] cleared inherited soft CPU-time limit\n";
      } else {
        log << "  [process] cannot clear soft CPU-time limit: "
            << std::strerror(errno) << '\n';
      }
    } else if (cpu.rlim_max != RLIM_INFINITY) {
      log << "  [process] finite CPU-time limit retained; if startup ends with "
             "SIGXCPU, adjust the launching service's LimitCPU or launch from "
             "a terminal with an unlimited CPU-time limit\n";
    }
  } else {
    log << "  [process] cannot read RLIMIT_CPU: " << std::strerror(errno) << '\n';
  }

  // Keep the real-time watchdog intact for any threads that deliberately opt
  // into real-time scheduling later (for example, an audio backend).
  rlimit realtime{};
  if (getrlimit(RLIMIT_RTTIME, &realtime) == 0) {
    LogLimit(log, "RLIMIT_RTTIME", realtime, "microseconds");
  } else {
    log << "  [process] cannot read RLIMIT_RTTIME: " << std::strerror(errno)
        << '\n';
  }
  ClearInheritedMimallocEnvironment(log);
  return log.str();
}

}  // namespace mocktail::runtime
