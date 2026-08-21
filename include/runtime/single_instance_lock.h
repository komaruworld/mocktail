#ifndef MOCKTAIL_RUNTIME_SINGLE_INSTANCE_LOCK_H_
#define MOCKTAIL_RUNTIME_SINGLE_INSTANCE_LOCK_H_

#include <filesystem>
#include <string>

#include "runtime/environment.h"
#include "runtime/runtime_paths.h"

namespace mocktail {
namespace runtime {

// Stable lowercase-hex namespace derived from the executable installation
// path. Separate source trees cannot steal each other's browser launches,
// while every process started from one installed binary shares one owner.
std::string CurrentExecutableInstanceKey();

// Holds a non-blocking advisory lock for one Mocktail game process. The
// descriptor remains open for the lifetime of this object, so the kernel also
// releases the lock after a crash or forced process termination.
class SingleInstanceLock final {
 public:
  enum class Status { kAcquired, kAlreadyRunning, kError };

  ~SingleInstanceLock();

  SingleInstanceLock(const SingleInstanceLock&) = delete;
  SingleInstanceLock& operator=(const SingleInstanceLock&) = delete;
  SingleInstanceLock(SingleInstanceLock&& other) noexcept;
  SingleInstanceLock& operator=(SingleInstanceLock&& other) noexcept;

  static SingleInstanceLock AcquireForCurrentUser();
  // Updater canaries use fully isolated data/cache/state roots and an
  // isolated lock so they can validate a candidate while the user's current
  // payload remains open. Normal launches use one lock per user and Mocktail
  // installation path.
  static SingleInstanceLock AcquireForLaunch(const Environment& environment,
                                             const RuntimePaths& paths);
  static SingleInstanceLock Acquire(const std::filesystem::path& lock_file);

  Status status() const { return status_; }
  bool acquired() const { return status_ == Status::kAcquired; }
  bool already_running() const { return status_ == Status::kAlreadyRunning; }
  const std::filesystem::path& path() const { return path_; }
  const std::string& error() const { return error_; }

 private:
  SingleInstanceLock(int descriptor, Status status, std::filesystem::path path,
                     std::string error);

  int descriptor_ = -1;
  Status status_ = Status::kError;
  std::filesystem::path path_;
  std::string error_;
};

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_SINGLE_INSTANCE_LOCK_H_
