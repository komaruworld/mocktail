#ifndef MOCKTAIL_VR_OPENXR_PREVIEW_H_
#define MOCKTAIL_VR_OPENXR_PREVIEW_H_

#include <array>
#include <cstdint>
#include <string>

namespace mocktail::vr {

struct PreviewOptions {
  // Zero keeps the scene open until the runtime or user requests exit.
  std::uint32_t frame_limit = 0;
};

struct PreviewResult {
  bool success = false;
  std::string message;
  std::uint64_t frames = 0;
  std::uint64_t rendered_frames = 0;
  std::uint64_t tracked_frames = 0;
  std::array<float, 3> head_position{};
  std::array<float, 4> head_orientation{0, 0, 0, 1};
};

// Runs a native Vulkan/OpenXR scene with a panel fixed in LOCAL space. The
// compositor projects it into both eyes. Roblox images/input are not connected.
// Call on the main thread: SIGINT/SIGTERM request a graceful session exit, and
// their previous handlers are restored before returning.
PreviewResult RunOpenXrPreview(const PreviewOptions& options = {});
std::string FormatPreviewResult(const PreviewResult& result);

}  // namespace mocktail::vr

#endif  // MOCKTAIL_VR_OPENXR_PREVIEW_H_
