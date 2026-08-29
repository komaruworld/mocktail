#include <vector>

#include <gtest/gtest.h>

#include "mocktail/graphics/graphics_backend.h"
#include "mocktail/graphics/system_egl_probe.h"

namespace mocktail::graphics {
namespace {

TEST(SystemEglProbeTest, ValidatesHostSystemEglOnRealWindow) {
  const BackendCapability capability = ProbeSystemEgl(SystemEglProbeOptions{});
  if (capability.state == CapabilityState::kUnavailable) {
    GTEST_SKIP() << "host system EGL/window unavailable: " << capability.detail;
  }
  EXPECT_EQ(capability.backend, GraphicsBackendKind::kSystemEgl);
  EXPECT_FALSE(capability.detail.empty());
}

TEST(SystemEglProbeTest, SoftwareIsLoadableWhenDisallowed) {
  SystemEglProbeOptions options;
  options.allow_software_device = false;
  const BackendCapability capability = ProbeSystemEgl(options);
  if (capability.state == CapabilityState::kUnavailable) {
    GTEST_SKIP() << "host system EGL/window unavailable: " << capability.detail;
  }
  if (capability.acceleration == HardwareAcceleration::kSoftware) {
    EXPECT_EQ(capability.state, CapabilityState::kLoadable);
  }
}

TEST(SystemEglProbeTest, SelectableThroughBackendPolicy) {
  const BackendCapability system_egl = ProbeSystemEgl(SystemEglProbeOptions{});
  if (system_egl.state == CapabilityState::kUnavailable) {
    GTEST_SKIP() << "host system EGL/window unavailable: " << system_egl.detail;
  }

  const std::vector<BackendCapability> capabilities = {system_egl};
  BackendSelectionPolicy policy;
  policy.requested = GraphicsBackendKind::kSystemEgl;
  policy.allow_fallback = false;
  policy.require_hardware_acceleration = false;
  policy.require_runtime_validation = false;

  const BackendSelection selection =
      SelectGraphicsBackend(policy, capabilities);
  ASSERT_TRUE(selection.status.ok()) << selection.detail;
  EXPECT_EQ(selection.backend, GraphicsBackendKind::kSystemEgl);
}

}  // namespace
}  // namespace mocktail::graphics
