#include "window/vulkan_present_progress_gate.h"

#include <gtest/gtest.h>

namespace mocktail {
namespace window {
namespace {

constexpr uint64_t kFirstFrameTicksNs = 1000;

TEST(VulkanPresentProgressGateTest, IgnoresProgressOutsideActiveLifetime) {
  VulkanPresentProgressGate gate;
  VulkanPresentStallReport report;

  gate.NotifyFramePresented(kFirstFrameTicksNs, 1);
  EXPECT_FALSE(gate.Poll(
      kFirstFrameTicksNs + VulkanPresentProgressGate::kStallThresholdNs, true,
      &report));

  ASSERT_TRUE(gate.Activate());
  EXPECT_FALSE(gate.Activate());
  gate.NotifyFramePresented(kFirstFrameTicksNs, 1);
  gate.Deactivate();
  EXPECT_FALSE(gate.Poll(
      kFirstFrameTicksNs + VulkanPresentProgressGate::kStallThresholdNs, true,
      &report));
}

TEST(VulkanPresentProgressGateTest, ReportsRenderStallBeforeHostPresentOnce) {
  VulkanPresentProgressGate gate;
  ASSERT_TRUE(gate.Activate());
  gate.NotifyFramePresented(kFirstFrameTicksNs, 240);

  VulkanPresentStallReport report;
  EXPECT_FALSE(gate.Poll(
      kFirstFrameTicksNs + VulkanPresentProgressGate::kStallThresholdNs - 1,
      true, &report));
  ASSERT_TRUE(gate.Poll(
      kFirstFrameTicksNs + VulkanPresentProgressGate::kStallThresholdNs, true,
      &report));
  EXPECT_EQ(report.stage, VulkanPresentStallStage::kBeforeHostPresent);
  EXPECT_EQ(report.frame_count, 240u);
  EXPECT_EQ(report.host_present_count, 0u);
  EXPECT_FALSE(gate.Poll(
      kFirstFrameTicksNs + VulkanPresentProgressGate::kStallThresholdNs + 1,
      true, &report));
}

TEST(VulkanPresentProgressGateTest, IdentifiesBlockedHostPresent) {
  VulkanPresentProgressGate gate;
  ASSERT_TRUE(gate.Activate());
  gate.NotifyFramePresented(kFirstFrameTicksNs, 7);
  constexpr uint64_t kHostCallTicksNs = 2000;
  gate.NotifyHostPresentBegin(kHostCallTicksNs);

  VulkanPresentStallReport report;
  ASSERT_TRUE(
      gate.Poll(kHostCallTicksNs + VulkanPresentProgressGate::kStallThresholdNs,
                true, &report));
  EXPECT_EQ(report.stage, VulkanPresentStallStage::kInsideHostPresent);
  EXPECT_EQ(report.frame_count, 7u);
  EXPECT_EQ(report.host_present_count, 1u);
}

TEST(VulkanPresentProgressGateTest,
     WorkerHostPresentRemainsSecondaryToRenderGap) {
  VulkanPresentProgressGate gate;
  ASSERT_TRUE(gate.Activate());
  constexpr uint64_t kRenderThreadId = 4040;
  constexpr uint64_t kWorkerThreadId = 4141;
  gate.NotifyFramePresented(kFirstFrameTicksNs, 8, kRenderThreadId);
  constexpr uint64_t kHostCallTicksNs = kFirstFrameTicksNs + 100;
  const uint64_t sequence =
      gate.NotifyHostPresentBegin(kHostCallTicksNs, kWorkerThreadId);
  ASSERT_NE(sequence, 0u);

  VulkanPresentStallReport report;
  ASSERT_TRUE(
      gate.Poll(kHostCallTicksNs +
                    VulkanPresentProgressGate::kStallThresholdNs,
                true, &report));
  EXPECT_EQ(report.stage, VulkanPresentStallStage::kBeforeHostPresent);
  EXPECT_EQ(report.call_sequence, 0u);
  EXPECT_STREQ(report.oldest_active_call_name, "vkQueuePresentKHR(host)");
  EXPECT_EQ(report.oldest_active_call_sequence, sequence);
  EXPECT_EQ(report.oldest_active_call_thread_id, kWorkerThreadId);
}

TEST(VulkanPresentProgressGateTest, OldHostEndCannotClearNewHostCall) {
  VulkanPresentProgressGate gate;
  ASSERT_TRUE(gate.Activate());
  constexpr uint64_t kRenderThreadId = 4241;
  gate.NotifyFramePresented(kFirstFrameTicksNs, 9, kRenderThreadId);
  const uint64_t old_sequence = gate.NotifyHostPresentBegin(
      kFirstFrameTicksNs + 50, kRenderThreadId);
  ASSERT_NE(old_sequence, 0u);
  constexpr uint64_t kNewHostCallTicksNs = kFirstFrameTicksNs + 100;
  const uint64_t new_sequence =
      gate.NotifyHostPresentBegin(kNewHostCallTicksNs, kRenderThreadId);
  ASSERT_NE(new_sequence, 0u);
  gate.NotifyHostPresentEnd(old_sequence);

  VulkanPresentStallReport report;
  ASSERT_TRUE(
      gate.Poll(kNewHostCallTicksNs +
                    VulkanPresentProgressGate::kStallThresholdNs,
                true, &report));
  EXPECT_EQ(report.stage, VulkanPresentStallStage::kInsideHostPresent);
  EXPECT_EQ(report.call_sequence, new_sequence);
  EXPECT_EQ(report.thread_id, kRenderThreadId);
}

TEST(VulkanPresentProgressGateTest, IdentifiesBlockedVulkanCallAndThread) {
  VulkanPresentProgressGate gate;
  ASSERT_TRUE(gate.Activate());
  gate.NotifyFramePresented(kFirstFrameTicksNs, 11);
  constexpr uint64_t kCallTicksNs = 3000;
  constexpr uint64_t kThreadId = 4242;
  const uint64_t sequence =
      gate.NotifyCallBegin("vkWaitForFences", kCallTicksNs, kThreadId);
  ASSERT_NE(sequence, 0u);

  VulkanPresentStallReport report;
  ASSERT_TRUE(
      gate.Poll(kCallTicksNs + VulkanPresentProgressGate::kStallThresholdNs,
                true, &report));
  EXPECT_EQ(report.stage, VulkanPresentStallStage::kInsideVulkanCall);
  EXPECT_STREQ(report.call_name, "vkWaitForFences");
  EXPECT_EQ(report.call_sequence, sequence);
  EXPECT_EQ(report.thread_id, kThreadId);
  EXPECT_EQ(report.frame_count, 11u);
}

TEST(VulkanPresentProgressGateTest, RecentCallDoesNotHideExistingRenderGap) {
  VulkanPresentProgressGate gate;
  ASSERT_TRUE(gate.Activate());
  gate.NotifyFramePresented(kFirstFrameTicksNs, 12);
  const uint64_t call_ticks =
      kFirstFrameTicksNs + VulkanPresentProgressGate::kStallThresholdNs - 100;
  const uint64_t sequence =
      gate.NotifyCallBegin("vkQueueSubmit", call_ticks, 5150);
  ASSERT_NE(sequence, 0u);

  VulkanPresentStallReport report;
  ASSERT_TRUE(gate.Poll(
      kFirstFrameTicksNs + VulkanPresentProgressGate::kStallThresholdNs, true,
      &report));
  EXPECT_EQ(report.stage, VulkanPresentStallStage::kBeforeHostPresent);
  EXPECT_EQ(report.elapsed_ns,
            VulkanPresentProgressGate::kStallThresholdNs);

  ASSERT_TRUE(gate.Poll(
      call_ticks + VulkanPresentProgressGate::kStallThresholdNs, true,
      &report));
  EXPECT_EQ(report.stage, VulkanPresentStallStage::kInsideVulkanCall);
  EXPECT_EQ(report.call_sequence, sequence);
}

TEST(VulkanPresentProgressGateTest,
     CallStartedBeforeLatestFrameIsSecondaryToRenderGap) {
  VulkanPresentProgressGate gate;
  ASSERT_TRUE(gate.Activate());
  constexpr uint64_t kWorkerThreadId = 6160;
  const uint64_t sequence =
      gate.NotifyCallBegin("vkWaitSemaphores", kFirstFrameTicksNs,
                           kWorkerThreadId);
  ASSERT_NE(sequence, 0u);
  constexpr uint64_t kLaterFrameTicksNs = kFirstFrameTicksNs + 1000;
  constexpr uint64_t kRenderThreadId = 6260;
  gate.NotifyFramePresented(kLaterFrameTicksNs, 13, kRenderThreadId);
  const uint64_t stalled_ticks =
      kLaterFrameTicksNs + VulkanPresentProgressGate::kStallThresholdNs;

  VulkanPresentStallReport report;
  ASSERT_TRUE(gate.Poll(stalled_ticks, true, &report));
  EXPECT_EQ(report.stage, VulkanPresentStallStage::kBeforeHostPresent);
  EXPECT_EQ(report.call_name, nullptr);
  EXPECT_EQ(report.call_sequence, 0u);
  EXPECT_EQ(report.thread_id, 0u);
  EXPECT_EQ(report.elapsed_ns,
            VulkanPresentProgressGate::kStallThresholdNs);
  EXPECT_EQ(report.last_present_thread_id, kRenderThreadId);
  EXPECT_STREQ(report.oldest_active_call_name, "vkWaitSemaphores");
  EXPECT_EQ(report.oldest_active_call_sequence, sequence);
  EXPECT_EQ(report.oldest_active_call_thread_id, kWorkerThreadId);
  EXPECT_EQ(report.oldest_active_call_elapsed_ns,
            stalled_ticks - kFirstFrameTicksNs);
}

TEST(VulkanPresentProgressGateTest,
     WorkerCallStartedAfterFrameRemainsSecondaryToRenderGap) {
  VulkanPresentProgressGate gate;
  ASSERT_TRUE(gate.Activate());
  constexpr uint64_t kRenderThreadId = 6360;
  constexpr uint64_t kWorkerThreadId = 6460;
  gate.NotifyFramePresented(kFirstFrameTicksNs, 14, kRenderThreadId);
  constexpr uint64_t kCallStartedNs = kFirstFrameTicksNs + 100;
  const uint64_t sequence = gate.NotifyCallBegin(
      "vkWaitSemaphores", kCallStartedNs, kWorkerThreadId);
  ASSERT_NE(sequence, 0u);

  VulkanPresentStallReport report;
  ASSERT_TRUE(gate.Poll(
      kCallStartedNs + VulkanPresentProgressGate::kStallThresholdNs, true,
      &report));
  EXPECT_EQ(report.stage, VulkanPresentStallStage::kBeforeHostPresent);
  EXPECT_EQ(report.call_sequence, 0u);
  EXPECT_EQ(report.thread_id, 0u);
  EXPECT_STREQ(report.oldest_active_call_name, "vkWaitSemaphores");
  EXPECT_EQ(report.oldest_active_call_sequence, sequence);
  EXPECT_EQ(report.oldest_active_call_thread_id, kWorkerThreadId);
}

TEST(VulkanPresentProgressGateTest,
     RenderCallStartedAfterFrameIsPrimaryStall) {
  VulkanPresentProgressGate gate;
  ASSERT_TRUE(gate.Activate());
  constexpr uint64_t kRenderThreadId = 6560;
  gate.NotifyFramePresented(kFirstFrameTicksNs, 15, kRenderThreadId);
  constexpr uint64_t kCallStartedNs = kFirstFrameTicksNs + 100;
  const uint64_t sequence = gate.NotifyCallBegin(
      "vkResetCommandPool", kCallStartedNs, kRenderThreadId);
  ASSERT_NE(sequence, 0u);

  VulkanPresentStallReport report;
  ASSERT_TRUE(gate.Poll(
      kCallStartedNs + VulkanPresentProgressGate::kStallThresholdNs, true,
      &report));
  EXPECT_EQ(report.stage, VulkanPresentStallStage::kInsideVulkanCall);
  EXPECT_STREQ(report.call_name, "vkResetCommandPool");
  EXPECT_EQ(report.call_sequence, sequence);
  EXPECT_EQ(report.thread_id, kRenderThreadId);
}

TEST(VulkanPresentProgressGateTest, ReportsEachNewBlockedCallInSameStall) {
  VulkanPresentProgressGate gate;
  ASSERT_TRUE(gate.Activate());
  gate.NotifyFramePresented(kFirstFrameTicksNs, 14);
  const uint64_t first_started_ns = kFirstFrameTicksNs + 100;
  const uint64_t first_sequence =
      gate.NotifyCallBegin("vkWaitForFences", first_started_ns, 7170);
  ASSERT_NE(first_sequence, 0u);

  VulkanPresentStallReport report;
  ASSERT_TRUE(gate.Poll(
      first_started_ns + VulkanPresentProgressGate::kStallThresholdNs, true,
      &report));
  EXPECT_EQ(report.call_sequence, first_sequence);
  gate.NotifyCallEnd(first_sequence, 0);
  EXPECT_FALSE(gate.Poll(
      first_started_ns + VulkanPresentProgressGate::kStallThresholdNs + 1,
      true, &report));

  const uint64_t second_started_ns =
      first_started_ns + VulkanPresentProgressGate::kStallThresholdNs + 100;
  const uint64_t second_sequence =
      gate.NotifyCallBegin("vkQueueWaitIdle", second_started_ns, 8180);
  ASSERT_NE(second_sequence, 0u);
  ASSERT_TRUE(gate.Poll(
      second_started_ns + VulkanPresentProgressGate::kStallThresholdNs, true,
      &report));
  EXPECT_EQ(report.stage, VulkanPresentStallStage::kInsideVulkanCall);
  EXPECT_EQ(report.call_sequence, second_sequence);
}

TEST(VulkanPresentProgressGateTest,
     RecentHostPresentDoesNotHideExistingRenderGap) {
  VulkanPresentProgressGate gate;
  ASSERT_TRUE(gate.Activate());
  gate.NotifyFramePresented(kFirstFrameTicksNs, 15);
  const uint64_t host_started_ns =
      kFirstFrameTicksNs + VulkanPresentProgressGate::kStallThresholdNs - 100;
  gate.NotifyHostPresentBegin(host_started_ns);

  VulkanPresentStallReport report;
  ASSERT_TRUE(gate.Poll(
      kFirstFrameTicksNs + VulkanPresentProgressGate::kStallThresholdNs, true,
      &report));
  EXPECT_EQ(report.stage, VulkanPresentStallStage::kBeforeHostPresent);

  ASSERT_TRUE(gate.Poll(
      host_started_ns + VulkanPresentProgressGate::kStallThresholdNs, true,
      &report));
  EXPECT_EQ(report.stage, VulkanPresentStallStage::kInsideHostPresent);
  EXPECT_EQ(report.host_present_count, 1u);
}

TEST(VulkanPresentProgressGateTest, CompletedCallDoesNotMaskRenderGap) {
  VulkanPresentProgressGate gate;
  ASSERT_TRUE(gate.Activate());
  gate.NotifyFramePresented(kFirstFrameTicksNs, 4);
  const uint64_t sequence =
      gate.NotifyCallBegin("vkQueueSubmit", kFirstFrameTicksNs + 100, 99);
  ASSERT_NE(sequence, 0u);
  gate.NotifyCallEnd(sequence, 0);

  VulkanPresentStallReport report;
  ASSERT_TRUE(gate.Poll(
      kFirstFrameTicksNs + VulkanPresentProgressGate::kStallThresholdNs, true,
      &report));
  EXPECT_EQ(report.stage, VulkanPresentStallStage::kBeforeHostPresent);
  EXPECT_EQ(report.call_name, nullptr);
  EXPECT_STREQ(report.last_completed_call_name, "vkQueueSubmit");
  EXPECT_EQ(report.last_completed_call_result, 0);
}

TEST(VulkanPresentProgressGateTest,
     ReportsCoherentRenderThreadCompletionMetadata) {
  VulkanPresentProgressGate gate;
  ASSERT_TRUE(gate.Activate());
  constexpr uint64_t kRenderThreadId = 9090;
  gate.NotifyFramePresented(kFirstFrameTicksNs, 16, kRenderThreadId);
  constexpr uint64_t kCallStartedNs = kFirstFrameTicksNs + 100;
  constexpr uint64_t kCallCompletedNs = kCallStartedNs + 50;
  constexpr int32_t kCallResult = -7;
  const uint64_t sequence =
      gate.NotifyCallBegin("vkResetFences", kCallStartedNs, kRenderThreadId);
  ASSERT_NE(sequence, 0u);
  gate.NotifyCallEnd(sequence, kCallResult, kCallCompletedNs, kRenderThreadId);

  VulkanPresentStallReport report;
  ASSERT_TRUE(gate.Poll(
      kFirstFrameTicksNs + VulkanPresentProgressGate::kStallThresholdNs, true,
      &report));
  EXPECT_EQ(report.stage, VulkanPresentStallStage::kBeforeHostPresent);
  EXPECT_EQ(report.last_present_thread_id, kRenderThreadId);
  EXPECT_STREQ(report.last_completed_call_name, "vkResetFences");
  EXPECT_EQ(report.last_completed_call_sequence, sequence);
  EXPECT_EQ(report.last_completed_call_thread_id, kRenderThreadId);
  EXPECT_EQ(report.last_completed_call_started_ns, kCallStartedNs);
  EXPECT_EQ(report.last_completed_call_completed_ns, kCallCompletedNs);
  EXPECT_EQ(report.last_completed_call_result, kCallResult);
}

TEST(VulkanPresentProgressGateTest,
     WorkerCompletionDoesNotOverwriteRenderThreadRecord) {
  VulkanPresentProgressGate gate;
  ASSERT_TRUE(gate.Activate());
  constexpr uint64_t kRenderThreadId = 9191;
  constexpr uint64_t kWorkerThreadId = 9292;
  gate.NotifyFramePresented(kFirstFrameTicksNs, 17, kRenderThreadId);

  constexpr uint64_t kRenderCallStartedNs = kFirstFrameTicksNs + 100;
  constexpr uint64_t kRenderCallCompletedNs = kRenderCallStartedNs + 50;
  constexpr int32_t kRenderCallResult = -3;
  const uint64_t render_sequence = gate.NotifyCallBegin(
      "vkResetCommandPool", kRenderCallStartedNs, kRenderThreadId);
  ASSERT_NE(render_sequence, 0u);
  gate.NotifyCallEnd(render_sequence, kRenderCallResult,
                     kRenderCallCompletedNs, kRenderThreadId);

  constexpr uint64_t kWorkerCallStartedNs = kRenderCallCompletedNs + 50;
  constexpr uint64_t kWorkerCallCompletedNs = kWorkerCallStartedNs + 50;
  const uint64_t worker_sequence = gate.NotifyCallBegin(
      "vkWaitSemaphores", kWorkerCallStartedNs, kWorkerThreadId);
  ASSERT_NE(worker_sequence, 0u);
  gate.NotifyCallEnd(worker_sequence, 0, kWorkerCallCompletedNs,
                     kWorkerThreadId);

  VulkanPresentStallReport report;
  ASSERT_TRUE(gate.Poll(
      kFirstFrameTicksNs + VulkanPresentProgressGate::kStallThresholdNs, true,
      &report));
  EXPECT_EQ(report.stage, VulkanPresentStallStage::kBeforeHostPresent);
  EXPECT_EQ(report.last_present_thread_id, kRenderThreadId);
  EXPECT_STREQ(report.last_completed_call_name, "vkResetCommandPool");
  EXPECT_EQ(report.last_completed_call_sequence, render_sequence);
  EXPECT_EQ(report.last_completed_call_thread_id, kRenderThreadId);
  EXPECT_EQ(report.last_completed_call_started_ns, kRenderCallStartedNs);
  EXPECT_EQ(report.last_completed_call_completed_ns,
            kRenderCallCompletedNs);
  EXPECT_EQ(report.last_completed_call_result, kRenderCallResult);
}

TEST(VulkanPresentProgressGateTest,
     CompletedCallFromOldLifecycleIsNotReported) {
  VulkanPresentProgressGate gate;
  ASSERT_TRUE(gate.Activate());
  constexpr uint64_t kRenderThreadId = 9393;
  gate.NotifyFramePresented(kFirstFrameTicksNs, 18, kRenderThreadId);
  const uint64_t sequence = gate.NotifyCallBegin(
      "vkResetFences", kFirstFrameTicksNs + 100, kRenderThreadId);
  ASSERT_NE(sequence, 0u);
  gate.NotifyCallEnd(sequence, 0, kFirstFrameTicksNs + 150,
                     kRenderThreadId);
  gate.Deactivate();

  ASSERT_TRUE(gate.Activate());
  constexpr uint64_t kNextFrameTicksNs = kFirstFrameTicksNs + 1000;
  gate.NotifyFramePresented(kNextFrameTicksNs, 1, kRenderThreadId);
  VulkanPresentStallReport report;
  ASSERT_TRUE(gate.Poll(
      kNextFrameTicksNs + VulkanPresentProgressGate::kStallThresholdNs, true,
      &report));
  EXPECT_EQ(report.stage, VulkanPresentStallStage::kBeforeHostPresent);
  EXPECT_TRUE(report.last_completed_call_snapshot_coherent);
  EXPECT_EQ(report.last_completed_call_name, nullptr);
  EXPECT_EQ(report.last_completed_call_sequence, 0u);
}

TEST(VulkanPresentProgressGateTest, FailedHostPresentIsNotFrameProgress) {
  VulkanPresentProgressGate gate;
  ASSERT_TRUE(gate.Activate());
  gate.NotifyFramePresented(kFirstFrameTicksNs, 5);
  const uint64_t sequence =
      gate.NotifyHostPresentBegin(kFirstFrameTicksNs + 100);
  ASSERT_NE(sequence, 0u);
  gate.NotifyHostPresentEnd(sequence);

  VulkanPresentStallReport report;
  ASSERT_TRUE(gate.Poll(
      kFirstFrameTicksNs + VulkanPresentProgressGate::kStallThresholdNs, true,
      &report));
  EXPECT_EQ(report.stage, VulkanPresentStallStage::kBeforeHostPresent);
  EXPECT_EQ(report.frame_count, 5u);
  EXPECT_EQ(report.host_present_count, 1u);
}

TEST(VulkanPresentProgressGateTest, NewProgressClearsReportedStall) {
  VulkanPresentProgressGate gate;
  ASSERT_TRUE(gate.Activate());
  gate.NotifyFramePresented(kFirstFrameTicksNs, 1);

  VulkanPresentStallReport report;
  ASSERT_TRUE(gate.Poll(
      kFirstFrameTicksNs + VulkanPresentProgressGate::kStallThresholdNs, true,
      &report));

  constexpr uint64_t kNextHostCallTicksNs = 7000000000ULL;
  const uint64_t sequence =
      gate.NotifyHostPresentBegin(kNextHostCallTicksNs);
  ASSERT_NE(sequence, 0u);
  gate.NotifyHostPresentEnd(sequence);
  gate.NotifyFramePresented(kNextHostCallTicksNs + 20, 2);
  ASSERT_TRUE(gate.Poll(
      kNextHostCallTicksNs + 20 + VulkanPresentProgressGate::kStallThresholdNs,
      true, &report));
  EXPECT_EQ(report.frame_count, 2u);
}

TEST(VulkanPresentProgressGateTest, SuspendedWindowDoesNotConsumeReport) {
  VulkanPresentProgressGate gate;
  ASSERT_TRUE(gate.Activate());
  gate.NotifyFramePresented(kFirstFrameTicksNs, 1);
  const uint64_t stalled_ticks =
      kFirstFrameTicksNs + VulkanPresentProgressGate::kStallThresholdNs;

  VulkanPresentStallReport report;
  EXPECT_FALSE(gate.Poll(stalled_ticks, false, &report));
  EXPECT_TRUE(gate.Poll(stalled_ticks, true, &report));
}

}  // namespace
}  // namespace window
}  // namespace mocktail
