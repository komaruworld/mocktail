#ifndef MOCKTAIL_RUNTIME_ROBLOX_EXPERIENCE_PRESENCE_H_
#define MOCKTAIL_RUNTIME_ROBLOX_EXPERIENCE_PRESENCE_H_

#include "runtime/roblox_experience_launch_bridge.h"

namespace mocktail {
namespace runtime {

enum class RobloxExperiencePresencePhase {
  kBrowsing,
  kJoining,
  kPlaying,
};

using NotifyRobloxExperiencePresenceFn =
    void (*)(void* context, RobloxExperiencePresencePhase phase,
             const RobloxExperienceLaunchRequest* request);

// Non-owning observer retained by RobloxExperienceComposition. Its owner must
// outlive the composition. Browsing notifications carry a null request.
struct RobloxExperiencePresenceObserver {
  void* context = nullptr;
  NotifyRobloxExperiencePresenceFn notify = nullptr;

  bool valid() const { return notify != nullptr; }
};

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_ROBLOX_EXPERIENCE_PRESENCE_H_
