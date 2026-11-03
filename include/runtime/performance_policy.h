#ifndef MOCKTAIL_RUNTIME_PERFORMANCE_POLICY_H_
#define MOCKTAIL_RUNTIME_PERFORMANCE_POLICY_H_

#include <cstdint>
#include <string>
#include <string_view>

#include "runtime/frame_rate_policy.h"
#include "runtime/game_mode.h"

namespace mocktail {
namespace runtime {

enum class PhysicsWorkerMode {
  kAuto,
  kLatency,
  kThroughput,
};

bool ParsePhysicsWorkerMode(std::string_view value, PhysicsWorkerMode* mode);
std::string_view PhysicsWorkerModeName(PhysicsWorkerMode mode);

// Selects which cores size Roblox's worker pools. On hybrid Intel CPUs
// (Alder Lake+, e.g. i5-1235U with 2 P-cores + 8 E-cores) `auto` sizes pools
// from the performance cores only; `all` keeps the legacy every-physical-core
// behavior; `performance` forces performance cores when the topology is
// known.
enum class WorkerCoreMode {
  kAuto,
  kAll,
  kPerformance,
};

bool ParseWorkerCoreMode(std::string_view value, WorkerCoreMode* mode);
std::string_view WorkerCoreModeName(WorkerCoreMode mode);

// Pure resolution for tests: the worker-sizing core count for a given mode
// and detected topology. Always at least one.
int ResolveWorkerSizingCoreCount(WorkerCoreMode mode, bool hybrid,
                                 int performance_threads,
                                 int all_physical_cores);

// Malformed topology still yields one worker.
int CalculateThroughputWorkerCount(int available_physical_cores);

struct PerformancePolicy {
  bool multithreaded_rendering = false;
  int physical_core_count = 0;
  std::uint64_t memory_limit_mb = 0;
  bool memory_limit_valid = true;
  GameModePolicy game_mode = GameModePolicy::kAuto;
  bool game_mode_valid = true;
  PhysicsWorkerMode physics_worker_mode = PhysicsWorkerMode::kAuto;
  bool physics_worker_mode_valid = true;
  WorkerCoreMode worker_core_mode = WorkerCoreMode::kAuto;
  bool worker_core_mode_valid = true;
  // Detected when worker pools are sized (multithreaded rendering or
  // throughput mode); zero/unknown otherwise.
  bool hybrid_cpu = false;
  int performance_threads = 0;

  bool memory_limit_enabled() const {
    return memory_limit_valid && memory_limit_mb != 0;
  }
  std::uint64_t memory_limit_bytes() const {
    return memory_limit_mb * 1024U * 1024U;
  }
};

PerformancePolicy ParsePerformancePolicy(
    std::string_view multithreaded_rendering,
    std::string_view memory_limit_mb = "0", std::string_view game_mode = "auto",
    std::string_view physics_worker_mode = "auto",
    std::string_view worker_cores = "auto");

// Respects CPU affinity and falls back to the available logical count.
int DetectAvailablePhysicalCoreCount();

// Disabled policy preserves overrides; conflicting values fail.
bool MergePerformanceClientSettingsOverrides(const PerformancePolicy& policy,
                                             std::string_view base_json,
                                             std::string* merged_json,
                                             std::string* error);

// Applies mandatory HTTP and crash policies after caller settings.
bool MergeRuntimeClientSettingsOverrides(const FrameRatePolicy& frame_rate,
                                         const PerformancePolicy& performance,
                                         std::string_view base_json,
                                         std::string* merged_json,
                                         std::string* error);

// Keeps the legacy test bypass disabled. The host PermissionsProtocol now
// answers microphone authorization from capture configuration. No permission
// query opens a recording device.
bool MergeAudioDeviceMenuClientSettingsOverrides(std::string_view base_json,
                                                 std::string *merged_json,
                                                 std::string *error);

bool MergeAudioCaptureClientSettingsOverrides(bool microphone_enabled,
                                              std::string_view base_json,
                                              std::string* merged_json,
                                              std::string* error);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_PERFORMANCE_POLICY_H_
