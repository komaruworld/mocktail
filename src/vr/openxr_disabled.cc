#include "mocktail/vr/openxr_probe.h"
#include "mocktail/vr/openxr_preview.h"

namespace mocktail::vr {

PreviewResult RunOpenXrPreview(const PreviewOptions&) {
  PreviewResult result;
  result.message =
      "This build does not include OpenXR support. Use a VR-enabled build of "
      "Mocktail.";
  return result;
}

ProbeReport ProbeOpenXr() {
  ProbeReport report;
  report.status = ProbeStatus::kBuildDisabled;
  report.message =
      "This build does not include OpenXR support. Use a "
      "VR-enabled build of Mocktail.";
  return report;
}

}  // namespace mocktail::vr
