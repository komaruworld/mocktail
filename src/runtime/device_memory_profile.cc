#include "runtime/device_memory_profile.h"

#include <algorithm>
#include <limits>

namespace mocktail {
namespace runtime {
namespace {

constexpr std::uint64_t kMebibyte = 1024U * 1024U;
constexpr std::uint64_t kMinimumNormalRamBytes = 4096U * kMebibyte;

}  // namespace

DeviceMemoryProfile BuildDeviceMemoryProfile(std::uint64_t host_memory_bytes) {
  DeviceMemoryProfile profile;
  if (host_memory_bytes < kMinimumNormalRamBytes) {
    profile.total_memory_mb = 2048;
    profile.memory_class_mb = 256;
    profile.large_memory_class_mb = 512;
    profile.low_memory_killer_background_threshold = 256;
    profile.low_memory_killer_foreground_threshold = 512;
    profile.low_ram_device = true;
    return profile;
  }
  profile.total_memory_mb = static_cast<std::int32_t>(std::min<std::uint64_t>(
      host_memory_bytes / kMebibyte,
      static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())));
  profile.memory_class_mb = 512;
  profile.large_memory_class_mb = 1024;
  return profile;
}

}  // namespace runtime
}  // namespace mocktail
