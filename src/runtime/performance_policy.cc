#include "runtime/performance_policy.h"

#include <sched.h>

#define JSON_NOEXCEPTION 1
#include <algorithm>
#include <array>
#include <charconv>
#include <fstream>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <thread>
#include <utility>

#include "runtime/crash_report_policy.h"
#include "runtime/http_client_policy.h"

namespace mocktail {
namespace runtime {
namespace {

struct ClientSetting {
  std::string_view name;
  std::string_view value;
};

bool SetCompatibleValue(nlohmann::json* object, const ClientSetting& setting,
                        std::string* error) {
  const std::string name(setting.name);
  const std::string value(setting.value);
  const auto existing = object->find(name);
  if (existing != object->end()) {
    if (!existing->is_string() || existing->get<std::string>() != value) {
      if (error != nullptr) {
        *error = std::string("performance policy conflicts with ") + name;
      }
      return false;
    }
    return true;
  }
  (*object)[name] = value;
  return true;
}

bool ReadTopologyValue(int logical_cpu, std::string_view name, int* value) {
  const std::string path = "/sys/devices/system/cpu/cpu" +
                           std::to_string(logical_cpu) + "/topology/" +
                           std::string(name);
  std::ifstream input(path);
  return static_cast<bool>(input >> *value);
}

int LogicalCpuFallback(const cpu_set_t* allowed) {
  int count = 0;
  if (allowed != nullptr) {
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
      if (CPU_ISSET(cpu, allowed)) {
        ++count;
      }
    }
  }
  if (count > 0) {
    return count;
  }
  return static_cast<int>(std::max(1U, std::thread::hardware_concurrency()));
}

}  // namespace

bool ParsePhysicsWorkerMode(std::string_view value, PhysicsWorkerMode* mode) {
  if (mode == nullptr) {
    return false;
  }
  if (value.empty() || value == "auto") {
    *mode = PhysicsWorkerMode::kAuto;
    return true;
  }
  if (value == "latency") {
    *mode = PhysicsWorkerMode::kLatency;
    return true;
  }
  if (value == "throughput") {
    *mode = PhysicsWorkerMode::kThroughput;
    return true;
  }
  return false;
}

std::string_view PhysicsWorkerModeName(PhysicsWorkerMode mode) {
  switch (mode) {
    case PhysicsWorkerMode::kAuto:
      return "auto";
    case PhysicsWorkerMode::kLatency:
      return "latency";
    case PhysicsWorkerMode::kThroughput:
      return "throughput";
  }
  return "auto";
}

int CalculateThroughputWorkerCount(int available_physical_cores) {
  return std::max(1, available_physical_cores);
}

PerformancePolicy ParsePerformancePolicy(
    std::string_view multithreaded_rendering, std::string_view memory_limit_mb,
    std::string_view game_mode, std::string_view physics_worker_mode) {
  PerformancePolicy policy;
  policy.multithreaded_rendering = multithreaded_rendering == "1" ||
                                   multithreaded_rendering == "true" ||
                                   multithreaded_rendering == "on";
  policy.physics_worker_mode_valid =
      ParsePhysicsWorkerMode(physics_worker_mode, &policy.physics_worker_mode);
  if (policy.multithreaded_rendering ||
      policy.physics_worker_mode == PhysicsWorkerMode::kThroughput) {
    policy.physical_core_count = DetectAvailablePhysicalCoreCount();
  }
  const char* memory_begin = memory_limit_mb.data();
  const char* memory_end = memory_begin + memory_limit_mb.size();
  const std::from_chars_result memory_parsed =
      std::from_chars(memory_begin, memory_end, policy.memory_limit_mb);
  policy.memory_limit_valid =
      memory_parsed.ec == std::errc() && memory_parsed.ptr == memory_end &&
      policy.memory_limit_mb <=
          std::numeric_limits<std::uint64_t>::max() / (1024U * 1024U);
  policy.game_mode_valid = ParseGameModePolicy(game_mode, &policy.game_mode);
  return policy;
}

int DetectAvailablePhysicalCoreCount() {
  cpu_set_t allowed;
  CPU_ZERO(&allowed);
  if (sched_getaffinity(0, sizeof(allowed), &allowed) != 0) {
    return LogicalCpuFallback(nullptr);
  }

  std::set<std::pair<int, int>> physical_cores;
  for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu) {
    if (!CPU_ISSET(cpu, &allowed)) {
      continue;
    }
    int package_id = 0;
    int core_id = 0;
    if (!ReadTopologyValue(cpu, "physical_package_id", &package_id) ||
        !ReadTopologyValue(cpu, "core_id", &core_id)) {
      return LogicalCpuFallback(&allowed);
    }
    physical_cores.emplace(package_id, core_id);
  }
  return physical_cores.empty() ? LogicalCpuFallback(&allowed)
                                : static_cast<int>(physical_cores.size());
}

bool MergePerformanceClientSettingsOverrides(const PerformancePolicy& policy,
                                             std::string_view base_json,
                                             std::string* merged_json,
                                             std::string* error) {
  if (merged_json == nullptr) {
    if (error != nullptr) {
      *error = "performance client-settings output is required";
    }
    return false;
  }
  nlohmann::json overrides = nlohmann::json::parse(
      base_json.empty() ? "{}" : base_json, nullptr, false, true);
  if (overrides.is_discarded() || !overrides.is_object()) {
    if (error != nullptr) {
      *error = "client-settings overrides must be a JSON object";
    }
    return false;
  }
  if (!policy.physics_worker_mode_valid) {
    if (error != nullptr) {
      *error = "physics worker mode is invalid";
    }
    return false;
  }
  if (!policy.multithreaded_rendering &&
      policy.physics_worker_mode == PhysicsWorkerMode::kAuto) {
    *merged_json = overrides.dump();
    return true;
  }
  const auto apply_settings = [&overrides,
                               error](const auto& settings) -> bool {
    for (const ClientSetting& setting : settings) {
      if (!SetCompatibleValue(&overrides, setting, error)) {
        return false;
      }
    }
    return true;
  };
  const bool throughput_mode =
      policy.physics_worker_mode == PhysicsWorkerMode::kThroughput;
  if (policy.physics_worker_mode == PhysicsWorkerMode::kLatency) {
    const std::array<ClientSetting, 1> physics_settings = {{
        {"DFIntSimMidPhaseContactPipelineBatchSize", "128"},
    }};
    if (!apply_settings(physics_settings)) {
      return false;
    }
  }
  int render_worker_count = 0;
  if (throughput_mode ||
      (policy.multithreaded_rendering &&
       policy.physics_worker_mode != PhysicsWorkerMode::kLatency)) {
    const int available_workers = policy.physical_core_count > 0
                                      ? policy.physical_core_count
                                      : DetectAvailablePhysicalCoreCount();
    render_worker_count = std::max(1, available_workers);
  }
  if (throughput_mode) {
    constexpr int kMaximumAsyncMinimum = 3;
    render_worker_count = CalculateThroughputWorkerCount(render_worker_count);
    const std::string workers = std::to_string(render_worker_count);
    const std::string async_minimum =
        std::to_string(std::min(render_worker_count, kMaximumAsyncMinimum));
    const std::array<ClientSetting, 4> physics_settings = {{
        {"FIntTaskSchedulerThreadMin", "0"},
        {"FIntTaskSchedulerAsyncTasksMinimumThreadCount", async_minimum},
        {"FIntTaskSchedulerAutoThreadLimit", workers},
        {"DFIntSimMidPhaseContactPipelineBatchSize", "128"},
    }};
    if (!apply_settings(physics_settings)) {
      return false;
    }
  } else if (policy.multithreaded_rendering &&
             policy.physics_worker_mode != PhysicsWorkerMode::kLatency) {
    const std::string workers = std::to_string(render_worker_count);
    const std::array<ClientSetting, 2> scheduler_settings = {{
        {"FIntTaskSchedulerThreadMin", workers},
        {"FIntTaskSchedulerAsyncTasksMinimumThreadCount", workers},
    }};
    if (!apply_settings(scheduler_settings)) {
      return false;
    }
  }
  if ((policy.multithreaded_rendering || throughput_mode) &&
      policy.physics_worker_mode != PhysicsWorkerMode::kLatency) {
    const std::string workers = std::to_string(render_worker_count);
    const std::string occlusion_workers =
        std::to_string(std::max(1, render_worker_count / 2));
    const std::array<ClientSetting, 4> rendering_settings = {{
        {"FIntSmoothClusterTaskQueueMaxParallelTasks", workers},
        {"FIntOcclusionWorkerThreadCount", occlusion_workers},
        {"FFlagMovePrerenderV2", "True"},
        {"FFlagGcInParallelWithRenderPrepare3", "True"},
    }};
    if (!apply_settings(rendering_settings)) {
      return false;
    }
  }
  *merged_json = overrides.dump();
  return true;
}

bool MergeRuntimeClientSettingsOverrides(const FrameRatePolicy& frame_rate,
                                         const PerformancePolicy& performance,
                                         std::string_view base_json,
                                         std::string* merged_json,
                                         std::string* error) {
  std::string frame_rate_overrides;
  if (!MergeFrameRateClientSettingsOverrides(frame_rate, base_json,
                                             &frame_rate_overrides, error)) {
    return false;
  }
  std::string performance_overrides;
  if (!MergePerformanceClientSettingsOverrides(
          performance, frame_rate_overrides, &performance_overrides, error)) {
    return false;
  }
  std::string http_client_overrides;
  if (!MergeHttpClientSettingsOverrides(performance_overrides,
                                        &http_client_overrides, error)) {
    return false;
  }
  return MergeCrashReportClientSettingsOverrides(http_client_overrides,
                                                 merged_json, error);
}

}  // namespace runtime
}  // namespace mocktail
