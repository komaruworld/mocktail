#ifndef MOCKTAIL_RUNTIME_MEMORY_LIMIT_H_
#define MOCKTAIL_RUNTIME_MEMORY_LIMIT_H_

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "runtime/owned_pthread.h"

namespace mocktail {
namespace runtime {

struct ProcessMemoryUsage {
  std::uint64_t resident_bytes = 0;
  std::uint64_t swap_bytes = 0;
  bool valid = false;

  std::uint64_t committed_bytes() const {
    return resident_bytes + swap_bytes;
  }
};

// Parses Linux /proc/<pid>/status memory fields. VmRSS is required; VmSwap is
// optional because some kernels omit it when swap support is disabled.
ProcessMemoryUsage ParseProcessStatusMemory(std::string_view status);

enum class CgroupMemoryLimitStatus {
  kActive,
  kUnavailable,
  kExecFailed,
};

struct CgroupMemoryLimitResult {
  CgroupMemoryLimitStatus status = CgroupMemoryLimitStatus::kUnavailable;
  std::string detail;

  bool active() const { return status == CgroupMemoryLimitStatus::kActive; }
};

// Compatibility hook for the former cgroup re-exec path. Automatic cgroup
// scope creation is disabled, so callers continue with the watchdog fallback.
CgroupMemoryLimitResult MaybeReexecWithCgroupMemoryLimit(
    int argc, char* const argv[], std::uint64_t limit_bytes,
    const std::vector<std::string>* reexec_arguments = nullptr);

// Portable fallback for Linux systems where a delegated cgroup cannot be
// created. It watches the current process's actual RSS plus swap and exits with
// status 137 before a runaway guest can keep exhausting host memory.
class MemoryLimitWatchdog final {
 public:
  MemoryLimitWatchdog() = default;
  ~MemoryLimitWatchdog();

  MemoryLimitWatchdog(const MemoryLimitWatchdog&) = delete;
  MemoryLimitWatchdog& operator=(const MemoryLimitWatchdog&) = delete;

  bool Start(std::uint64_t limit_bytes, std::string* error);
  void Stop();

  bool running() const { return running_; }

 private:
  static void* ThreadEntry(void* context);
  void Run();

  std::atomic<bool> stop_requested_{false};
  std::uint64_t limit_bytes_ = 0;
  OwnedPthread worker_;
  bool running_ = false;
};

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_MEMORY_LIMIT_H_
