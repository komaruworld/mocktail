#ifndef MOCKTAIL_RUNTIME_ROBLOX_LAUNCH_URI_H_
#define MOCKTAIL_RUNTIME_ROBLOX_LAUNCH_URI_H_

#include <cstddef>
#include <string_view>

#include "mocktail/status.h"
#include "runtime/roblox_experience_launch_bridge.h"

namespace mocktail {
namespace runtime {

inline constexpr std::size_t kMaximumRobloxLaunchUriBytes = 64 * 1024;

// Converts the two Roblox website launch protocols into the owned
// ExperienceProtocol contract used by the supported game-session pipeline.
// Authentication tickets and tracker IDs are discarded; the runtime already
// has a validated account identity.
Status ParseRobloxLaunchUri(std::string_view uri,
                            RobloxExperienceLaunchRequest* request);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_ROBLOX_LAUNCH_URI_H_
