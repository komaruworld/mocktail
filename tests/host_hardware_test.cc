#include "runtime/host_hardware.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace mocktail {
namespace runtime {
namespace {

// Synthetic 12th Gen Intel Core i5-1235U: 2 P-cores with SMT (logical CPUs
// 0-3, 4.40 GHz) and 8 E-cores (logical CPUs 4-11, 3.30 GHz).
std::vector<CpuInfoEntry> MakeI5_1235UEntries() {
  std::vector<CpuInfoEntry> entries;
  for (int cpu = 0; cpu < 4; ++cpu) {
    entries.push_back(CpuInfoEntry{cpu, 0, cpu / 2, 4400000});
  }
  for (int cpu = 4; cpu < 12; ++cpu) {
    entries.push_back(CpuInfoEntry{cpu, 0, cpu, 3300000});
  }
  return entries;
}

TEST(ParseCpuListTest, ParsesRangesSingletonsAndNoise) {
  EXPECT_EQ(ParseCpuList(""), std::vector<int>({}));
  EXPECT_EQ(ParseCpuList("0-3,8,10-11"),
            std::vector<int>({0, 1, 2, 3, 8, 10, 11}));
  EXPECT_EQ(ParseCpuList("5"), std::vector<int>({5}));
  EXPECT_EQ(ParseCpuList(" 2 , 4-5 "), std::vector<int>({2, 4, 5}));
  EXPECT_EQ(ParseCpuList("junk"), std::vector<int>({}));
  EXPECT_EQ(ParseCpuList("3-1"), std::vector<int>({}));
}

TEST(CpuTopologyTest, KernelListsClassifyAlderLakeHybrid) {
  const CpuTopology topology = ClassifyCpuTopology(
      "12th Gen Intel(R) Core(TM) i5-1235U", MakeI5_1235UEntries(),
      ParseCpuList("0-3"), ParseCpuList("4-11"));
  EXPECT_TRUE(topology.intel);
  EXPECT_TRUE(topology.hybrid);
  EXPECT_EQ(topology.logical_cores, 12);
  EXPECT_EQ(topology.physical_cores, 10);
  EXPECT_EQ(topology.performance_threads, 4);
  EXPECT_EQ(topology.performance_cores, 2);
  EXPECT_EQ(topology.efficiency_cores, 8);
  EXPECT_EQ(topology.max_freq_mhz, 4400);
}

TEST(CpuTopologyTest, FrequencyClusteringFallbackDetectsHybridIntel) {
  // No kernel cpu_core/cpu_atom lists: fall back to max-freq clustering.
  const CpuTopology topology =
      ClassifyCpuTopology("12th Gen Intel(R) Core(TM) i5-1235U",
                          MakeI5_1235UEntries(), {}, {});
  EXPECT_TRUE(topology.hybrid);
  EXPECT_EQ(topology.performance_threads, 4);
  EXPECT_EQ(topology.performance_cores, 2);
  EXPECT_EQ(topology.efficiency_cores, 8);
}

TEST(CpuTopologyTest, UniformFrequenciesAreNotHybrid) {
  std::vector<CpuInfoEntry> entries;
  for (int cpu = 0; cpu < 8; ++cpu) {
    entries.push_back(CpuInfoEntry{cpu, 0, cpu / 2, 3400000});
  }
  const CpuTopology topology = ClassifyCpuTopology(
      "Intel(R) Core(TM) i7-8700 CPU @ 3.20GHz", entries, {}, {});
  EXPECT_TRUE(topology.intel);
  EXPECT_FALSE(topology.hybrid);
  EXPECT_EQ(topology.physical_cores, 4);
  EXPECT_EQ(topology.logical_cores, 8);
  EXPECT_EQ(topology.performance_cores, 0);
}

TEST(CpuTopologyTest, NonIntelMultiClusterIsNotTreatedAsHybrid) {
  const CpuTopology topology = ClassifyCpuTopology(
      "AMD Ryzen 9 7945HX", MakeI5_1235UEntries(), {}, {});
  EXPECT_FALSE(topology.intel);
  EXPECT_FALSE(topology.hybrid);
  EXPECT_EQ(topology.physical_cores, 10);
}

TEST(CpuTopologyTest, MissingCpufreqFallsBackToNonHybrid) {
  std::vector<CpuInfoEntry> entries;
  for (int cpu = 0; cpu < 12; ++cpu) {
    entries.push_back(CpuInfoEntry{cpu, 0, cpu, 0});
  }
  const CpuTopology topology = ClassifyCpuTopology(
      "12th Gen Intel(R) Core(TM) i5-1235U", entries, {}, {});
  EXPECT_FALSE(topology.hybrid);
  EXPECT_EQ(topology.max_freq_mhz, 0);
}

TEST(GpuClassifyTest, IntelIrisXeIsIntegratedIgpu) {
  const GpuInfo gpu = ClassifyGpu("0x8086", "0x46a8", "i915",
                                  "Iris Xe Graphics");
  EXPECT_TRUE(gpu.present);
  EXPECT_TRUE(gpu.intel_igpu);
  EXPECT_TRUE(gpu.integrated);
  EXPECT_EQ(gpu.vendor_id, "8086");
  EXPECT_EQ(gpu.device_id, "46a8");
}

TEST(GpuClassifyTest, IntelArcIsDiscrete) {
  const GpuInfo gpu =
      ClassifyGpu("8086", "56a5", "xe", "Intel Arc A770 Graphics");
  EXPECT_TRUE(gpu.present);
  EXPECT_FALSE(gpu.intel_igpu);
  EXPECT_FALSE(gpu.integrated);
}

TEST(GpuClassifyTest, VirtualGpusShareSystemMemory) {
  EXPECT_TRUE(ClassifyGpu("0x1af4", "0x1050", "virtio_gpu", "virtio").integrated);
  EXPECT_TRUE(ClassifyGpu("", "", "llvmpipe", "llvmpipe").integrated);
  EXPECT_FALSE(ClassifyGpu("0x1002", "0x744c", "amdgpu", "AMD Radeon").integrated);
  EXPECT_FALSE(ClassifyGpu("0x10de", "0x2684", "nvidia", "NVIDIA RTX").integrated);
  EXPECT_FALSE(ClassifyGpu("", "", "", "").present);
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
