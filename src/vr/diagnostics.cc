#include "mocktail/vr/openxr_probe.h"
#include "mocktail/vr/openxr_preview.h"

#include <sstream>

namespace mocktail::vr {

std::string FormatPreviewResult(const PreviewResult& result) {
  std::ostringstream output;
  output << "[VR] " << result.message << '\n'
         << "Frames: " << result.frames
         << ", rendered: " << result.rendered_frames
         << ", tracked: " << result.tracked_frames << '\n';
  if (result.tracked_frames) {
    output << "Head position (metres): " << result.head_position[0] << ", "
           << result.head_position[1] << ", " << result.head_position[2] << '\n'
           << "Head orientation (xyzw): " << result.head_orientation[0] << ", "
           << result.head_orientation[1] << ", " << result.head_orientation[2]
           << ", " << result.head_orientation[3] << '\n';
  }
  return output.str();
}

int ProbeExitCode(ProbeStatus status) {
  switch (status) {
    case ProbeStatus::kReady:
      return 0;
    case ProbeStatus::kBuildDisabled:
      return 2;
    case ProbeStatus::kRuntimeUnavailable:
      return 3;
    case ProbeStatus::kHeadsetUnavailable:
      return 4;
    case ProbeStatus::kIncompatibleRuntime:
      return 5;
    case ProbeStatus::kError:
      return 1;
  }
  return 1;
}

std::string FormatProbeReport(const ProbeReport& report) {
  std::ostringstream output;
  output << "Mocktail OpenXR discovery\n";
  if (!report.sdk_version.empty())
    output << "SDK: " << report.sdk_version << '\n';
  if (!report.runtime_name.empty()) {
    output << "Runtime: " << report.runtime_name << " ("
           << report.runtime_version << ")\n";
  }
  if (!report.system_name.empty()) {
    output << "System: " << report.system_name << '\n'
           << "Tracking: orientation=" << report.orientation_tracking
           << " position=" << report.position_tracking << '\n';
  }
  for (std::size_t eye = 0; eye < report.views.size(); ++eye) {
    const auto& view = report.views[eye];
    output << "Eye " << eye << ": " << view.width << 'x' << view.height
           << ", samples=" << view.samples << '\n';
  }
  if (!report.minimum_vulkan_version.empty()) {
    output << "Vulkan: minimum=" << report.minimum_vulkan_version
           << ", highest runtime-tested=" << report.maximum_vulkan_version
           << '\n';
  }
  output << "Extensions (" << report.extensions.size() << "):\n";
  for (const auto& extension : report.extensions)
    output << "  " << extension << '\n';
  output << "Discovery: " << report.message << '\n'
         << "Roblox VR rendering: not implemented yet.\n"
         << "This probe does not create an XR session or render frames.\n";
  return output.str();
}

}  // namespace mocktail::vr
