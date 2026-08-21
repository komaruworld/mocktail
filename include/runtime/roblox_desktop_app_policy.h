#ifndef MOCKTAIL_RUNTIME_ROBLOX_DESKTOP_APP_POLICY_H_
#define MOCKTAIL_RUNTIME_ROBLOX_DESKTOP_APP_POLICY_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace mocktail {
namespace runtime {

struct DesktopAppPolicyResult {
  bool app_storage_created = false;
  bool updated = false;
  std::size_t normalized_policy_count = 0;
  std::string policy_json;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

// Normalizes only Roblox UniversalApp layout coordinates for a PC profile.
// Account eligibility and feature-entitlement fields remain server-owned.
// The guest and authenticated cache keys are both populated so user-switch
// startup cannot fall back to the Android Unknown/mobile layout.
DesktopAppPolicyResult ApplyDesktopAppPolicy(
    const std::filesystem::path& app_storage_file,
    const std::filesystem::path& default_policy_file,
    std::int64_t authenticated_user_id);

// Routes the normalized policy through Roblox's supported runtime override so
// a later GUAC response cannot replace the in-memory desktop layout with the
// Android Unknown layout after startup.
bool MergeDesktopAppPolicyClientSettingsOverride(std::string_view policy_json,
                                                 std::string_view base_json,
                                                 std::string* merged_json,
                                                 std::string* error);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_ROBLOX_DESKTOP_APP_POLICY_H_
