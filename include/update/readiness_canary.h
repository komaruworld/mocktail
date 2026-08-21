#ifndef MOCKTAIL_UPDATE_READINESS_CANARY_H_
#define MOCKTAIL_UPDATE_READINESS_CANARY_H_

#include <filesystem>
#include <string>
#include <string_view>

namespace mocktail::update {

enum class CanaryGraphicsBackend {
  kDirectVulkan,
  kOpenGlEs,
  kSystemEgl,
  kAngleVulkan,
  kAngleSwiftShader,
};

std::string_view CanaryGraphicsBackendName(CanaryGraphicsBackend backend);

// An empty value preserves the production direct-Vulkan default. Every EGL
// backend is validated through real OpenGL ES swap evidence.
bool ParseCanaryGraphicsBackend(std::string_view name,
                                CanaryGraphicsBackend* backend);

struct CanaryOptions {
  std::filesystem::path runtime_binary;
  std::filesystem::path payload_directory;
  std::filesystem::path compatibility_manifest;
  std::filesystem::path host_abi_profile;
  std::filesystem::path cache_root;
  std::filesystem::path state_root;
  CanaryGraphicsBackend graphics_backend =
      CanaryGraphicsBackend::kDirectVulkan;
  int timeout_seconds = 150;

  bool probation_candidate() const { return !host_abi_profile.empty(); }
};

struct CanaryResult {
  std::filesystem::path log_path;
  int exit_code = -1;
  std::string error;

  explicit operator bool() const {
    return error.empty() && !log_path.empty() && exit_code == 0;
  }
};

CanaryResult RunReadinessCanary(const CanaryOptions& options);

bool ValidateReadinessLog(CanaryGraphicsBackend backend, std::string_view log,
                          std::string* error);

}  // namespace mocktail::update

#endif  // MOCKTAIL_UPDATE_READINESS_CANARY_H_
