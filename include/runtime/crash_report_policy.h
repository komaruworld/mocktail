#ifndef MOCKTAIL_RUNTIME_CRASH_REPORT_POLICY_H_
#define MOCKTAIL_RUNTIME_CRASH_REPORT_POLICY_H_

#include <string>
#include <string_view>

namespace mocktail {
namespace runtime {

// Disables Roblox crash uploads while preserving unrelated overrides.
bool MergeCrashReportClientSettingsOverrides(std::string_view base_json,
                                             std::string* merged_json,
                                             std::string* error);

// AppBridge accepts only the FastVariable subset of this policy.
bool MergeCrashReportFastFlagsOverrides(std::string_view base_json,
                                        std::string* merged_json,
                                        std::string* error);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_CRASH_REPORT_POLICY_H_
