#include "mocktail/vr/openxr_probe.h"
#include "mocktail/vr/openxr_preview.h"

#include <charconv>
#include <iostream>
#include <string_view>

int main(int argc, char* argv[]) {
  if (argc == 2 && (std::string_view(argv[1]) == "--help" ||
                    std::string_view(argv[1]) == "-h")) {
    std::cout
        << "Usage: mocktail-vr-probe [--scene [--frames N]]\n"
           "Inspect the host OpenXR runtime and HMD capabilities.\n"
           "Uses the active runtime, or XR_RUNTIME_JSON if set.\n"
           "Without arguments: discovery only, no XR session.\n"
           "--scene: render a tracked test panel in a real Vulkan/XR session.\n"
           "--frames N: request exit after N frames (positive integer).\n"
           "Ctrl+C or the runtime closes the scene. Does not load Roblox.\n"
           "Exit codes: 0=discovery ready, 1=API error, 2=build disabled,\n"
           "3=runtime unavailable, 4=HMD unavailable, 5=incompatible "
           "runtime.\n";
    return 0;
  }
  if (argc >= 2 && std::string_view(argv[1]) == "--scene") {
    mocktail::vr::PreviewOptions options;
    if (argc == 4 && std::string_view(argv[2]) == "--frames") {
      const std::string_view value(argv[3]);
      const auto parsed = std::from_chars(
          value.data(), value.data() + value.size(), options.frame_limit);
      if (parsed.ec != std::errc() ||
          parsed.ptr != value.data() + value.size() || !options.frame_limit) {
        std::cerr << "--frames requires a positive integer.\n";
        return 1;
      }
    } else if (argc != 2) {
      std::cerr << "Use --scene [--frames N].\n";
      return 1;
    }
    const auto result = mocktail::vr::RunOpenXrPreview(options);
    std::cout << mocktail::vr::FormatPreviewResult(result);
    return result.success ? 0 : 1;
  }
  if (argc != 1) {
    std::cerr << "Use mocktail-vr-probe --help for supported options.\n";
    return 1;
  }
  const auto report = mocktail::vr::ProbeOpenXr();
  std::cout << mocktail::vr::FormatProbeReport(report);
  return mocktail::vr::ProbeExitCode(report.status);
}
