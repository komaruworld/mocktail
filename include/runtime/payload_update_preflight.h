#ifndef MOCKTAIL_RUNTIME_PAYLOAD_UPDATE_PREFLIGHT_H_
#define MOCKTAIL_RUNTIME_PAYLOAD_UPDATE_PREFLIGHT_H_

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "runtime/environment.h"
#include "runtime/runtime_paths.h"

namespace mocktail {
namespace runtime {

struct PayloadUpdatePreflightResult {
  bool attempted = false;
  std::string error;
  // Last failure the updater reported on its own stderr. Empty when the
  // updater never ran or never explained itself.
  std::string details;

  explicit operator bool() const { return error.empty(); }
};

PayloadUpdatePreflightResult RunPayloadUpdatePreflight(
    const Environment& environment, const RuntimePaths& paths,
    bool force_run_latest = false);

class BackgroundPayloadUpdate final {
 public:
  ~BackgroundPayloadUpdate();
  void Start(const Environment& environment, const RuntimePaths& paths,
             std::chrono::milliseconds interval = std::chrono::minutes(30));
  void Stop();

 private:
  std::mutex mutex_;
  std::condition_variable wake_;
  std::thread worker_;
  bool stopped_ = false;
  int child_ = -1;
};

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_PAYLOAD_UPDATE_PREFLIGHT_H_
