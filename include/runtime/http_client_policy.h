#ifndef MOCKTAIL_RUNTIME_HTTP_CLIENT_POLICY_H_
#define MOCKTAIL_RUNTIME_HTTP_CLIENT_POLICY_H_

#include <string>
#include <string_view>

namespace mocktail {
namespace runtime {

// Applies the supported Android HttpClient compatibility mode. The policy
// keeps endpoint-specific retries intact while disabling the generic LuaApp
// retry source and Roblox's RuntimeMutexRv backend, whose busy retry loop and
// lock traffic are disproportionately expensive across the Bionic bridge.
bool MergeHttpClientSettingsOverrides(std::string_view base_json,
                                      std::string* merged_json,
                                      std::string* error);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_HTTP_CLIENT_POLICY_H_
