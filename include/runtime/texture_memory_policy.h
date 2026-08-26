#ifndef MOCKTAIL_RUNTIME_TEXTURE_MEMORY_POLICY_H_
#define MOCKTAIL_RUNTIME_TEXTURE_MEMORY_POLICY_H_

#include <cstdint>
#include <string>
#include <string_view>

namespace mocktail {
namespace runtime {

// Roblox sizes its texture budget for the Android guest, so a PC host still
// gets caps.videoMemory = 64 MiB. These publish a host-sized budget through
// Roblox's own FIntRenderForceVideoMemorySize override.

// Total usable host memory in bytes, or 0 when it cannot be read.
std::uint64_t DetectHostMemoryBytes();

// 0 leaves Roblox's budget alone; otherwise an eighth of host memory, clamped
// to a PC-class range.
std::uint64_t CalculateTextureMemoryBudgetBytes(std::uint64_t host_memory_bytes);

// A 0 budget, or a key already present, is preserved: an explicit fflags.json
// or environment override wins.
bool MergeTextureMemoryClientSettingsOverrides(std::uint64_t budget_bytes,
                                               std::string_view base_json,
                                               std::string* merged_json,
                                               std::string* error);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_TEXTURE_MEMORY_POLICY_H_
