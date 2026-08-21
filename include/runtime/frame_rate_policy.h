#ifndef MOCKTAIL_RUNTIME_FRAME_RATE_POLICY_H_
#define MOCKTAIL_RUNTIME_FRAME_RATE_POLICY_H_

#include <string>
#include <string_view>

namespace mocktail {
namespace runtime {

inline constexpr int kMaximumSupportedRobloxSchedulerFps = 240;

enum class FrameRateLimitMode {
  kDisplay,
  kFixed,
  kUnlimited,
  kInvalid,
};

struct FrameRatePolicy {
  FrameRateLimitMode mode = FrameRateLimitMode::kDisplay;
  int fixed_fps = 0;

  bool valid() const { return mode != FrameRateLimitMode::kInvalid; }
};

FrameRatePolicy ParseFrameRatePolicy(std::string_view value);

// Merges the scheduler policy into Roblox's supported client-settings ingress.
// Unlimited selects the unmodified payload's maximum scheduler target (240).
// With VSync auto/off, graphics policy separately requests an unthrottled
// Vulkan present mode when the host exposes one.
// Existing unrelated overrides are retained. A conflicting explicit override
// is rejected instead of silently choosing one source of truth.
bool MergeFrameRateClientSettingsOverrides(const FrameRatePolicy &policy,
                                           std::string_view base_json,
                                           std::string *merged_json,
                                           std::string *error);

} // namespace runtime
} // namespace mocktail

#endif // MOCKTAIL_RUNTIME_FRAME_RATE_POLICY_H_
