#ifndef MOCKTAIL_VR_OPENXR_PROBE_H_
#define MOCKTAIL_VR_OPENXR_PROBE_H_

#include <cstdint>
#include <string>
#include <vector>

namespace mocktail::vr {

enum class ProbeStatus {
  kReady,
  kBuildDisabled,
  kRuntimeUnavailable,
  kHeadsetUnavailable,
  kIncompatibleRuntime,
  kError,
};

struct ViewConfiguration {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t samples = 0;
};

struct ProbeReport {
  ProbeStatus status = ProbeStatus::kError;
  std::string message;
  std::string sdk_version;
  std::string runtime_name;
  std::string runtime_version;
  std::string system_name;
  std::vector<std::string> extensions;
  std::vector<ViewConfiguration> views;
  bool orientation_tracking = false;
  bool position_tracking = false;
  std::string minimum_vulkan_version;
  std::string maximum_vulkan_version;
};

// Native host discovery only. Does not load Roblox, change the active runtime,
// create a graphics device/session, or claim that VR rendering is implemented.
ProbeReport ProbeOpenXr();
std::string FormatProbeReport(const ProbeReport& report);
int ProbeExitCode(ProbeStatus status);

}  // namespace mocktail::vr

#endif  // MOCKTAIL_VR_OPENXR_PROBE_H_
