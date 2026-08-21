#ifndef MOCKTAIL_RUNTIME_SUPPORT_BUNDLE_H_
#define MOCKTAIL_RUNTIME_SUPPORT_BUNDLE_H_

#include <filesystem>
#include <string>
#include <string_view>

#include "runtime/environment.h"
#include "runtime/runtime_paths.h"

namespace mocktail {
namespace runtime {

struct SupportBundleCollectionResult {
  bool attempted = false;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

// Creates a private native support directory. Context and reason are
// machine-readable non-sensitive tokens; this code owns log redaction and
// retention policy and never starts a shell or external collector.
SupportBundleCollectionResult CollectFailureSupportBundle(
    const Environment& environment, const RuntimePaths& paths,
    std::string_view context, std::string_view reason, int exit_code,
    const std::filesystem::path& source_log = {});

// Arms automatic diagnostics for the normal launch command. Every regular
// return while armed collects a bundle; successful shutdown explicitly
// disarms it. Fatal signals cannot safely fork from a signal handler and are
// covered by the outer readiness/updater harnesses instead.
class FailureSupportBundleGuard {
 public:
  FailureSupportBundleGuard(const Environment& environment,
                            const RuntimePaths& paths, bool armed);
  ~FailureSupportBundleGuard();

  FailureSupportBundleGuard(const FailureSupportBundleGuard&) = delete;
  FailureSupportBundleGuard& operator=(const FailureSupportBundleGuard&) =
      delete;

  void SetExitCode(int exit_code);
  void Disarm();

 private:
  const Environment* environment_ = nullptr;
  const RuntimePaths* paths_ = nullptr;
  int exit_code_ = 1;
  bool armed_ = false;
};

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_SUPPORT_BUNDLE_H_
