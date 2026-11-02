#ifndef MOCKTAIL_RUNTIME_HOST_HARDWARE_H_
#define MOCKTAIL_RUNTIME_HOST_HARDWARE_H_

#include <string>
#include <string_view>
#include <vector>

namespace mocktail {
namespace runtime {

// Parses a kernel CPU list such as "0-3,8,10-11" into logical CPU ids.
std::vector<int> ParseCpuList(std::string_view text);

struct CpuInfoEntry {
  int logical_id = 0;
  int package_id = 0;
  int core_id = 0;
  long max_freq_khz = 0;
};

// Hybrid-aware CPU topology. On Intel Alder Lake and newer (for example the
// 12th Gen i5-1235U: 2 P-cores with SMT at 4.40 GHz + 8 E-cores at 3.30 GHz,
// 10 cores / 12 threads) performance_* describes the P-core cluster.
struct CpuTopology {
  std::string model;
  int logical_cores = 0;
  int physical_cores = 0;
  int performance_threads = 0;
  int performance_cores = 0;
  int efficiency_cores = 0;
  bool hybrid = false;
  bool intel = false;
  long max_freq_mhz = 0;
};

// Pure classifier; tests feed synthetic topologies. Kernel-classified CPU
// lists (/sys/devices/cpu_core/cpus, /sys/devices/cpu_atom/cpus) win; when
// absent, Intel hybrids are detected by clustering per-CPU max frequencies
// (P-cores clock higher than E-cores).
CpuTopology ClassifyCpuTopology(
    std::string_view model, const std::vector<CpuInfoEntry>& entries,
    const std::vector<int>& kernel_performance_cpus,
    const std::vector<int>& kernel_efficiency_cpus);

// Production detection: /proc/cpuinfo model name, sched_getaffinity, and sysfs
// topology/cpufreq per allowed logical CPU.
CpuTopology DetectCpuTopology();

struct GpuInfo {
  bool present = false;
  std::string vendor_id;
  std::string device_id;
  std::string driver;
  std::string name;
  // True when the GPU has no dedicated VRAM and shares host memory (Intel
  // iGPU such as Iris Xe via i915/xe, virtual GPUs, or small SoC GPUs).
  bool integrated = false;
  bool intel_igpu = false;
};

// Pure classifier; tests feed synthetic PCI identities.
GpuInfo ClassifyGpu(std::string_view vendor_id, std::string_view device_id,
                    std::string_view driver, std::string_view name);

// Reads the first render node under /sys/class/drm.
GpuInfo DetectPrimaryGpu();

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_HOST_HARDWARE_H_
