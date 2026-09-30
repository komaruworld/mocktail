#ifndef MOCKTAIL_RUNTIME_PAYLOAD_UPDATE_PREFLIGHT_H_
#define MOCKTAIL_RUNTIME_PAYLOAD_UPDATE_PREFLIGHT_H_

#include <string>

#include "runtime/environment.h"
#include "runtime/runtime_paths.h"

namespace mocktail {
namespace runtime {

struct MocktailUpdateNotice {
  std::string heading;
  std::string body;
  std::string command;

  bool empty() const { return heading.empty() && body.empty(); }
};

struct PayloadUpdatePreflightResult {
  bool attempted = false;
  std::string error;
  // Last failure the updater reported on its own stderr. Empty when the
  // updater never ran or never explained itself.
  std::string details;
  MocktailUpdateNotice notice;

  explicit operator bool() const { return error.empty(); }
};

PayloadUpdatePreflightResult RunPayloadUpdatePreflight(
    const Environment& environment, const RuntimePaths& paths,
    bool force_run_latest = false);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_PAYLOAD_UPDATE_PREFLIGHT_H_
