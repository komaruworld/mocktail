#ifndef MOCKTAIL_RUNTIME_ROBLOX_FULLSCREEN_RUNTIME_BRIDGE_INTERNAL_H_
#define MOCKTAIL_RUNTIME_ROBLOX_FULLSCREEN_RUNTIME_BRIDGE_INTERNAL_H_

#include <cstddef>
#include <cstdint>

namespace mocktail {
namespace runtime {
namespace internal {

// Verifies the stable semantic instructions of the current setter without
// depending on relocation-specific call displacements.
bool HasExpectedFullscreenSetterContract(const std::uint8_t* code,
                                         std::size_t size);

}  // namespace internal
}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_ROBLOX_FULLSCREEN_RUNTIME_BRIDGE_INTERNAL_H_
