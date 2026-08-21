#include "runtime/performance_policy.h"

#include <gtest/gtest.h>

#define JSON_NOEXCEPTION 1
#include <nlohmann/json.hpp>
#include <string>

namespace mocktail {
namespace runtime {
namespace {

TEST(PerformancePolicyTest, ParsesDisabledByDefaultAndExplicitTrueValues) {
  EXPECT_FALSE(ParsePerformancePolicy("").multithreaded_rendering);
  EXPECT_FALSE(ParsePerformancePolicy("0").multithreaded_rendering);
  EXPECT_FALSE(ParsePerformancePolicy("false").multithreaded_rendering);
  EXPECT_FALSE(ParsePerformancePolicy("off").multithreaded_rendering);
  EXPECT_TRUE(ParsePerformancePolicy("1").multithreaded_rendering);
  EXPECT_TRUE(ParsePerformancePolicy("true").multithreaded_rendering);
  EXPECT_TRUE(ParsePerformancePolicy("on").multithreaded_rendering);
  EXPECT_EQ(ParsePerformancePolicy("0").physical_core_count, 0);
  EXPECT_GT(ParsePerformancePolicy("true").physical_core_count, 0);
  EXPECT_GT(DetectAvailablePhysicalCoreCount(), 0);
}

TEST(PerformancePolicyTest, ParsesPhysicsWorkerMode) {
  PhysicsWorkerMode mode = PhysicsWorkerMode::kLatency;
  EXPECT_TRUE(ParsePhysicsWorkerMode("auto", &mode));
  EXPECT_EQ(mode, PhysicsWorkerMode::kAuto);
  EXPECT_TRUE(ParsePhysicsWorkerMode("latency", &mode));
  EXPECT_EQ(mode, PhysicsWorkerMode::kLatency);
  EXPECT_TRUE(ParsePhysicsWorkerMode("throughput", &mode));
  EXPECT_EQ(mode, PhysicsWorkerMode::kThroughput);
  EXPECT_FALSE(ParsePhysicsWorkerMode("maximum", &mode));
  EXPECT_FALSE(ParsePhysicsWorkerMode("latency", nullptr));
  EXPECT_EQ(PhysicsWorkerModeName(PhysicsWorkerMode::kAuto), "auto");
  EXPECT_EQ(PhysicsWorkerModeName(PhysicsWorkerMode::kLatency), "latency");
  EXPECT_EQ(PhysicsWorkerModeName(PhysicsWorkerMode::kThroughput),
            "throughput");

  const PerformancePolicy latency =
      ParsePerformancePolicy("0", "0", "auto", "latency");
  EXPECT_TRUE(latency.physics_worker_mode_valid);
  EXPECT_EQ(latency.physics_worker_mode, PhysicsWorkerMode::kLatency);
  EXPECT_EQ(latency.physical_core_count, 0);
  const PerformancePolicy throughput =
      ParsePerformancePolicy("0", "0", "auto", "throughput");
  EXPECT_TRUE(throughput.physics_worker_mode_valid);
  EXPECT_EQ(throughput.physics_worker_mode, PhysicsWorkerMode::kThroughput);
  EXPECT_GT(throughput.physical_core_count, 0);
  EXPECT_FALSE(ParsePerformancePolicy("0", "0", "auto", "fast")
                   .physics_worker_mode_valid);
}

TEST(PerformancePolicyTest, MakesEveryPhysicalCoreAvailableToThroughputMode) {
  EXPECT_EQ(CalculateThroughputWorkerCount(1), 1);
  EXPECT_EQ(CalculateThroughputWorkerCount(2), 2);
  EXPECT_EQ(CalculateThroughputWorkerCount(4), 4);
  EXPECT_EQ(CalculateThroughputWorkerCount(8), 8);
  EXPECT_EQ(CalculateThroughputWorkerCount(14), 14);
  EXPECT_EQ(CalculateThroughputWorkerCount(32), 32);
}

TEST(PerformancePolicyTest, ParsesOptionalMemoryLimitInMebibytes) {
  const PerformancePolicy disabled = ParsePerformancePolicy("0", "0");
  EXPECT_TRUE(disabled.memory_limit_valid);
  EXPECT_FALSE(disabled.memory_limit_enabled());
  EXPECT_EQ(disabled.memory_limit_bytes(), 0U);

  const PerformancePolicy limited = ParsePerformancePolicy("0", "6144");
  EXPECT_TRUE(limited.memory_limit_valid);
  EXPECT_TRUE(limited.memory_limit_enabled());
  EXPECT_EQ(limited.memory_limit_mb, 6144U);
  EXPECT_EQ(limited.memory_limit_bytes(), 6144ULL * 1024ULL * 1024ULL);

  EXPECT_FALSE(ParsePerformancePolicy("0", "-1").memory_limit_valid);
  EXPECT_FALSE(ParsePerformancePolicy("0", "6GiB").memory_limit_valid);
  EXPECT_FALSE(ParsePerformancePolicy("0", "").memory_limit_valid);
  EXPECT_FALSE(
      ParsePerformancePolicy("0", "18446744073709551615").memory_limit_valid);
}

TEST(PerformancePolicyTest, DisabledPolicyPreservesUnrelatedOverrides) {
  std::string merged;
  std::string error;
  ASSERT_TRUE(MergePerformanceClientSettingsOverrides(
      {}, R"({"FStringGraphicsVulkanShaderMTDenyPattern":"4318:.*"})", &merged,
      &error))
      << error;

  const nlohmann::json parsed =
      nlohmann::json::parse(merged, nullptr, false, true);
  ASSERT_TRUE(parsed.is_object());
  EXPECT_EQ(parsed.at("FStringGraphicsVulkanShaderMTDenyPattern"), "4318:.*");
  EXPECT_FALSE(
      parsed.contains("FIntTaskSchedulerAsyncTasksMinimumThreadCount"));
}

TEST(PerformancePolicyTest, SizesFullMultithreadingFromPhysicalCoreCount) {
  std::string merged;
  std::string error;
  ASSERT_TRUE(MergePerformanceClientSettingsOverrides(
      {true, 14}, R"({"DFIntTaskSchedulerTargetFps":"240"})", &merged, &error))
      << error;

  const nlohmann::json parsed =
      nlohmann::json::parse(merged, nullptr, false, true);
  ASSERT_TRUE(parsed.is_object());
  EXPECT_EQ(parsed.at("DFIntTaskSchedulerTargetFps"), "240");
  EXPECT_EQ(parsed.at("FIntTaskSchedulerThreadMin"), "14");
  EXPECT_EQ(parsed.at("FIntTaskSchedulerAsyncTasksMinimumThreadCount"), "14");
  EXPECT_EQ(parsed.at("FIntSmoothClusterTaskQueueMaxParallelTasks"), "14");
  EXPECT_EQ(parsed.at("FIntOcclusionWorkerThreadCount"), "7");
  EXPECT_EQ(parsed.at("FFlagMovePrerenderV2"), "True");
  EXPECT_EQ(parsed.at("FFlagGcInParallelWithRenderPrepare3"), "True");
  EXPECT_FALSE(parsed.contains("FIntTaskSchedulerAutoThreadLimit"));
  EXPECT_FALSE(parsed.contains("FFlagTaskSchedulerLimitTargetFpsTo2402"));
}

TEST(PerformancePolicyTest, AcceptsIdenticalExplicitModeValues) {
  std::string merged;
  std::string error;
  ASSERT_TRUE(MergePerformanceClientSettingsOverrides(
      {true, 14},
      R"({"FIntOcclusionWorkerThreadCount":"7","FFlagMovePrerenderV2":"True"})",
      &merged, &error))
      << error;

  const nlohmann::json parsed =
      nlohmann::json::parse(merged, nullptr, false, true);
  ASSERT_TRUE(parsed.is_object());
  EXPECT_EQ(parsed.at("FIntOcclusionWorkerThreadCount"), "7");
  EXPECT_EQ(parsed.at("FFlagMovePrerenderV2"), "True");
}

TEST(PerformancePolicyTest, LatencyModePreservesRobloxManagedWorkerPools) {
  PerformancePolicy policy =
      ParsePerformancePolicy("true", "0", "auto", "latency");
  policy.physical_core_count = 14;
  std::string merged;
  std::string error;
  ASSERT_TRUE(
      MergePerformanceClientSettingsOverrides(policy, "{}", &merged, &error))
      << error;

  const nlohmann::json parsed =
      nlohmann::json::parse(merged, nullptr, false, true);
  ASSERT_TRUE(parsed.is_object());
  EXPECT_EQ(parsed.at("DFIntSimMidPhaseContactPipelineBatchSize"), "128");
  EXPECT_FALSE(parsed.contains("FIntTaskSchedulerThreadMin"));
  EXPECT_FALSE(parsed.contains("FIntTaskSchedulerAutoThreadLimit"));
  EXPECT_FALSE(parsed.contains("FIntSmoothClusterTaskQueueMaxParallelTasks"));
  EXPECT_FALSE(parsed.contains("FIntOcclusionWorkerThreadCount"));
}

TEST(PerformancePolicyTest, ThroughputModeUsesEveryPhysicalCore) {
  PerformancePolicy policy =
      ParsePerformancePolicy("false", "0", "auto", "throughput");
  policy.physical_core_count = 14;
  std::string merged;
  std::string error;
  ASSERT_TRUE(
      MergePerformanceClientSettingsOverrides(policy, "{}", &merged, &error))
      << error;

  const nlohmann::json parsed =
      nlohmann::json::parse(merged, nullptr, false, true);
  EXPECT_EQ(parsed.at("FIntTaskSchedulerThreadMin"), "0");
  EXPECT_EQ(parsed.at("FIntTaskSchedulerAutoThreadLimit"), "14");
  EXPECT_EQ(parsed.at("FIntTaskSchedulerAsyncTasksMinimumThreadCount"), "3");
  EXPECT_EQ(parsed.at("DFIntSimMidPhaseContactPipelineBatchSize"), "128");
  EXPECT_EQ(parsed.at("FIntSmoothClusterTaskQueueMaxParallelTasks"), "14");
  EXPECT_EQ(parsed.at("FIntOcclusionWorkerThreadCount"), "7");
}

TEST(PerformancePolicyTest, ComposesFrameRateBeforePerformanceMode) {
  std::string merged;
  std::string error;
  ASSERT_TRUE(MergeRuntimeClientSettingsOverrides(
      ParseFrameRatePolicy("144"), {true, 14},
      R"({"FStringGraphicsVulkanShaderMTDenyPattern":"4318:.*"})", &merged,
      &error))
      << error;

  const nlohmann::json parsed =
      nlohmann::json::parse(merged, nullptr, false, true);
  ASSERT_TRUE(parsed.is_object());
  EXPECT_EQ(parsed.at("DFIntTaskSchedulerTargetFps"), "144");
  EXPECT_EQ(parsed.at("FIntTaskSchedulerAsyncTasksMinimumThreadCount"), "14");
  EXPECT_EQ(parsed.at("FStringGraphicsVulkanShaderMTDenyPattern"), "4318:.*");
  EXPECT_EQ(parsed.at("FFlagUseRuntimeMutexRvHttpClient"), "False");
  EXPECT_EQ(parsed.at("DFFlagHttpClientSkipRetryForStreamingRequests"), "True");
  EXPECT_EQ(parsed.at("FFlagLuaAppDefaultHttpRetry"), "False");
}

TEST(PerformancePolicyTest, RejectsConflictingOrMalformedOverrides) {
  std::string merged = "unchanged";
  std::string error;
  EXPECT_FALSE(MergePerformanceClientSettingsOverrides(
      {true, 14}, R"({"FIntOcclusionWorkerThreadCount":"2"})", &merged,
      &error));
  EXPECT_EQ(merged, "unchanged");
  EXPECT_NE(error.find("FIntOcclusionWorkerThreadCount"), std::string::npos);

  error.clear();
  EXPECT_FALSE(MergePerformanceClientSettingsOverrides(
      {true, 14}, R"({"FFlagMovePrerenderV2":true})", &merged, &error));
  EXPECT_EQ(merged, "unchanged");
  EXPECT_NE(error.find("FFlagMovePrerenderV2"), std::string::npos);

  error.clear();
  EXPECT_FALSE(MergePerformanceClientSettingsOverrides({true, 14}, "not-json",
                                                       &merged, &error));
  EXPECT_EQ(merged, "unchanged");
  EXPECT_NE(error.find("JSON object"), std::string::npos);

  error.clear();
  EXPECT_FALSE(MergePerformanceClientSettingsOverrides({true, 14}, "[]",
                                                       &merged, &error));
  EXPECT_EQ(merged, "unchanged");
  EXPECT_NE(error.find("JSON object"), std::string::npos);

  PerformancePolicy latency =
      ParsePerformancePolicy("true", "0", "auto", "latency");
  latency.physical_core_count = 14;
  error.clear();
  EXPECT_FALSE(MergePerformanceClientSettingsOverrides(
      latency, R"({"DFIntSimMidPhaseContactPipelineBatchSize":"64"})", &merged,
      &error));
  EXPECT_NE(error.find("DFIntSimMidPhaseContactPipelineBatchSize"),
            std::string::npos);
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
