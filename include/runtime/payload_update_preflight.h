#ifndef MOCKTAIL_RUNTIME_PAYLOAD_UPDATE_PREFLIGHT_H_
#define MOCKTAIL_RUNTIME_PAYLOAD_UPDATE_PREFLIGHT_H_

#include <string>

#include "runtime/environment.h"
#include "runtime/runtime_paths.h"

namespace mocktail {
namespace runtime {

struct PayloadUpdatePreflightResult {
  bool attempted = false;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

PayloadUpdatePreflightResult RunPayloadUpdatePreflight(
    const Environment& environment, const RuntimePaths& paths);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_PAYLOAD_UPDATE_PREFLIGHT_H_
