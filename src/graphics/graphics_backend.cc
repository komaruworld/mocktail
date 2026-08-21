#include "mocktail/graphics/graphics_backend.h"

#include <string>

namespace mocktail {
namespace graphics {
namespace {

const BackendCapability* FindCapability(
    const std::vector<BackendCapability>& capabilities,
    GraphicsBackendKind backend) {
  for (const BackendCapability& capability : capabilities) {
    if (capability.backend == backend) {
      return &capability;
    }
  }
  return nullptr;
}

bool IsUsable(const BackendSelectionPolicy& policy,
              const BackendCapability& capability) {
  if (capability.state == CapabilityState::kUnavailable) {
    return false;
  }
  if (policy.require_runtime_validation &&
      capability.state != CapabilityState::kReady) {
    return false;
  }
  if (policy.require_hardware_acceleration &&
      capability.acceleration != HardwareAcceleration::kHardware) {
    return false;
  }
  return true;
}

BackendSelection Selected(const BackendCapability& capability) {
  BackendSelection result;
  result.status = Status::Ok();
  result.backend = capability.backend;
  result.detail = capability.detail;
  return result;
}

}  // namespace

BackendSelection SelectGraphicsBackend(
    const BackendSelectionPolicy& policy,
    const std::vector<BackendCapability>& capabilities) {
  const BackendCapability* requested =
      FindCapability(capabilities, policy.requested);
  if (requested != nullptr && IsUsable(policy, *requested)) {
    return Selected(*requested);
  }

  std::string reason = "requested graphics backend ";
  reason += GraphicsBackendName(policy.requested);
  if (requested == nullptr) {
    reason += " was not probed";
  } else if (!requested->detail.empty()) {
    reason += " is unavailable: ";
    reason += requested->detail;
  } else {
    reason += " did not satisfy the production capability policy";
  }

  if (!policy.allow_fallback) {
    return {Status::Error(StatusCode::kUnavailable, reason), policy.requested,
            reason};
  }

  constexpr GraphicsBackendKind kFallbackOrder[] = {
      GraphicsBackendKind::kAngleVulkan,
      GraphicsBackendKind::kDirectVulkan,
      GraphicsBackendKind::kSystemEgl,
  };
  for (GraphicsBackendKind backend : kFallbackOrder) {
    if (backend == policy.requested) {
      continue;
    }
    const BackendCapability* capability = FindCapability(capabilities, backend);
    if (capability != nullptr && IsUsable(policy, *capability)) {
      return Selected(*capability);
    }
  }

  reason += "; no explicitly allowed fallback satisfies the policy";
  return {Status::Error(StatusCode::kUnavailable, reason), policy.requested,
          reason};
}

const char* GraphicsBackendName(GraphicsBackendKind backend) {
  switch (backend) {
    case GraphicsBackendKind::kAngleVulkan:
      return "angle-vulkan";
    case GraphicsBackendKind::kDirectVulkan:
      return "direct-vulkan";
    case GraphicsBackendKind::kSystemEgl:
      return "system-egl";
  }
  return "unknown";
}

}  // namespace graphics
}  // namespace mocktail
