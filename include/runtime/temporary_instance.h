#ifndef MOCKTAIL_RUNTIME_TEMPORARY_INSTANCE_H_
#define MOCKTAIL_RUNTIME_TEMPORARY_INSTANCE_H_

#include <filesystem>
#include <string>

#include "runtime/environment.h"
#include "runtime/runtime_paths.h"

namespace mocktail {
namespace runtime {

struct TemporaryInstanceResult {
  std::filesystem::path root;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

// Redirects this process's data, cache, state, and auth roots to a fresh
// private directory under <primary state>/instances and binds the active
// managed payload through explicit environment paths so the guest can boot
// without shared state. The user configuration stays shared. A repeated call
// inside an already-redirected process (for example after a cgroup re-exec)
// returns the existing root instead of allocating another.
TemporaryInstanceResult PrepareTemporaryInstance(
    const Environment& environment, const RuntimePaths& primary_paths);

// Owns a prepared temporary root and removes it on destruction. The lock
// file inside the root keeps it alive for the stale-instance sweeper until
// the owning process exits.
class TemporaryInstance final {
 public:
  TemporaryInstance() = default;
  explicit TemporaryInstance(std::filesystem::path root);
  ~TemporaryInstance();

  TemporaryInstance(const TemporaryInstance&) = delete;
  TemporaryInstance& operator=(const TemporaryInstance&) = delete;
  TemporaryInstance(TemporaryInstance&& other) noexcept;
  TemporaryInstance& operator=(TemporaryInstance&& other) noexcept;

  const std::filesystem::path& root() const { return root_; }
  bool active() const { return !root_.empty(); }
  void Remove();

 private:
  std::filesystem::path root_;
};

// Reads the resolved runtime configuration for the current process and
// reports whether additional launches should become temporary instances.
// The multi_instance.temporary_windows YAML key maps to
// MOCKTAIL_TEMP_INSTANCE_WINDOWS; an unset value means false.
bool TemporaryInstanceWindowsEnabled(
    const Environment& environment,
    const std::filesystem::path& config_file);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_TEMPORARY_INSTANCE_H_
