#include <gtest/gtest.h>
#include <openxr/openxr.h>
#include "mocktail/vr/openxr_backend.h"
#include "mocktail/vr/roblox_vr_device_bridge.h"

namespace mocktail::vr {
namespace internal {
struct VrBridgeTestAccess {
  static void Enable(RobloxVrDeviceBridge& bridge) { bridge.active_ = true; }
};
}
struct OpenXrBackendTestAccess {
  static size_t DesktopCount(const OpenXrBackend& b) { return b.desktop_swapchains_.size(); }
  template<class T> static T Handle(std::uintptr_t n) { return reinterpret_cast<T>(n); }
  static void Configure(OpenXrBackend& backend) {
    backend.vk_instance_ = Handle<VkInstance>(1);
    backend.vk_device_ = Handle<VkDevice>(2);
    backend.recording_ = true;
    backend.published_pose_.frame = 7;
    backend.published_pose_.valid = true;
  }
  static VkInstance Instance(const OpenXrBackend& b) { return b.vk_instance_; }
  static void GraphicsFamily(OpenXrBackend& b, unsigned family) {
    b.vk_queue_family_ = family;
    b.vk_graphics_queue_ = Handle<VkQueue>(30);
    b.device_queue_families_ = {family, family + 1};
  }
  static VkQueue CopyQueue(const OpenXrBackend& b, VkQueue present) {
    return b.GraphicsQueueForPresentLocked(present);
  }
  static void NextFrame(OpenXrBackend& b) { ++b.published_pose_.frame; }
  static bool SupportsQueue(const OpenXrBackend& b, VkQueue queue) {
    return b.SupportsPresentQueueLocked(queue);
  }
  static void Attachment(OpenXrBackend& b, int id, void* owner, VkDevice device) {
    auto image = Handle<VkImage>(id);
    auto& r = b.images_[image];
    r.image = image; r.device = device; r.owner = owner;
    r.width = r.height = 500; r.format = VK_FORMAT_R8G8B8A8_UNORM;
    r.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    b.image_views_[Handle<VkImageView>(id)] = image;
    b.framebuffer_views_[Handle<VkFramebuffer>(id)] = {Handle<VkImageView>(id)};
  }
  static void Pass(
      OpenXrBackend& b, RobloxVrDeviceBridge& bridge, int eye, int id,
      void* owner,
      VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
      VkCommandBuffer command = VK_NULL_HANDLE) {
    bridge.OnEyeGetterCall(owner, eye, Handle<void*>(id));
    VkRenderPassBeginInfo info{};
    info.framebuffer = Handle<VkFramebuffer>(id);
    info.renderPass = Handle<VkRenderPass>(99);
    b.render_pass_final_layouts_[info.renderPass] = {layout};
    b.NoteRenderPassBegin(command, &info);
  }
  static VkImage Image(const OpenXrBackend& b, int eye) { return b.eyes_[eye].image; }
  static void AddMsaaAttachment(OpenXrBackend& b, int framebuffer, int msaa) {
    b.images_[Handle<VkImage>(msaa)].samples = VK_SAMPLE_COUNT_4_BIT;
    b.framebuffer_views_[Handle<VkFramebuffer>(framebuffer)].insert(
        b.framebuffer_views_[Handle<VkFramebuffer>(framebuffer)].begin(),
        Handle<VkImageView>(msaa));
  }
  static void ResolvedPass(OpenXrBackend& b, RobloxVrDeviceBridge& bridge,
                           void* owner, int framebuffer) {
    bridge.OnEyeGetterCall(owner, 0, Handle<void*>(framebuffer));
    VkRenderPassBeginInfo info{};
    info.framebuffer = Handle<VkFramebuffer>(framebuffer);
    info.renderPass = Handle<VkRenderPass>(99);
    b.render_pass_final_layouts_[info.renderPass] = {
        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    b.NoteRenderPassBegin(VK_NULL_HANDLE, &info);
  }
  static VkImageLayout Layout(const OpenXrBackend& b, int eye) { return b.eyes_[eye].final_layout; }
  static void Invalidate(OpenXrBackend& b) {
    b.InvalidateInFlightPose("test");
  }
  static void SetFrameOpen(OpenXrBackend& b, bool open) { b.frame_open_ = open; }
  static void SetVisibilityLost(OpenXrBackend& b, bool lost) { b.visibility_lost_ = lost; }
  static bool VisibilityLost(const OpenXrBackend& b) { return b.visibility_lost_; }
  static void NotePose(OpenXrBackend& b, void* owner) {
    b.NotePoseApplied(owner, b.published_pose_.frame);
  }
  static std::size_t AppliedPoses(const OpenXrBackend& b) {
    return b.applied_poses_.size();
  }
  static void State(OpenXrBackend& b, XrSessionState state) { b.HandleSessionState(state); }
  static void ReferenceChange(OpenXrBackend& b, std::int64_t time) { b.HandleReferenceSpaceChange(time); }
  static void FrameAt(OpenXrBackend& b, std::uint64_t time) {
    b.frame_open_ = true; b.frame_views_valid_ = true; b.frame_display_time_ = time;
    b.published_pose_.valid = true;
  }
  static bool Recovering(OpenXrBackend& b) { return b.session_recovery_pending_; }
  static VkDevice Device(OpenXrBackend& b) { return b.vk_device_; }
  static std::size_t Resources(OpenXrBackend& b) { return b.images_.size(); }
  static void SeedResource(OpenXrBackend& b) { b.images_[Handle<VkImage>(99)] = {}; }
  static void LatchProjectionRejection(OpenXrBackend& b) {
    b.projection_rejection_logged_ = true;
  }
  static bool ProjectionRejectionLogged(const OpenXrBackend& b) {
    return b.projection_rejection_logged_;
  }
};
using Access = OpenXrBackendTestAccess;
TEST(OpenXrBackendLifecycle, DeviceDestructionPreservesLiveInstance) {
  OpenXrBackend b;
  Access::Configure(b);
  b.NoteDeviceDestroyed(Access::Handle<VkDevice>(3));
  EXPECT_TRUE(b.WantsResourceRecords());
  b.NoteDeviceDestroyed(Access::Handle<VkDevice>(2));
  EXPECT_EQ(Access::Instance(b), Access::Handle<VkInstance>(1));
  EXPECT_FALSE(b.WantsResourceRecords());
  EXPECT_FALSE(b.PublishedHeadPose().valid);
  b.NoteInstanceDestroyed(Access::Handle<VkInstance>(1));
  EXPECT_EQ(Access::Instance(b), VK_NULL_HANDLE);
}
TEST(OpenXrBackendBinding, SwitchesOwnerWithoutWaitingForOldResourcesToDie) {
  OpenXrBackend b;
  RobloxVrDeviceBridge bridge;
  internal::VrBridgeTestAccess::Enable(bridge);
  Access::Configure(b);
  auto* first = Access::Handle<void*>(100);
  auto* second = Access::Handle<void*>(200);
  for (int id = 10; id < 14; ++id)
    Access::Attachment(b, id, id < 12 ? first : second, Access::Handle<VkDevice>(2));
  Access::Pass(b, bridge, 0, 10, first);
  Access::Pass(b, bridge, 1, 11, first);
  ASSERT_TRUE(b.EyeBindingComplete());
  Access::Pass(b, bridge, 0, 12, second);
  EXPECT_FALSE(b.EyeBindingComplete());
  Access::Pass(b, bridge, 1, 13, second);
  EXPECT_TRUE(b.EyeBindingComplete());
  EXPECT_EQ(Access::Image(b, 0), Access::Handle<VkImage>(12));
  EXPECT_EQ(Access::Image(b, 1), Access::Handle<VkImage>(13));
  Access::Pass(b, bridge, 0, 12, second, VK_IMAGE_LAYOUT_GENERAL);
  EXPECT_EQ(Access::Layout(b, 0), VK_IMAGE_LAYOUT_GENERAL);
}
TEST(OpenXrBackendBinding, RejectsAnotherDeviceImages) {
  OpenXrBackend b;
  RobloxVrDeviceBridge bridge;
  internal::VrBridgeTestAccess::Enable(bridge);
  Access::Configure(b);
  auto* owner = Access::Handle<void*>(100);
  Access::Attachment(b, 10, owner, Access::Handle<VkDevice>(3));
  Access::Pass(b, bridge, 0, 10, owner);
  EXPECT_EQ(Access::Image(b, 0), VK_NULL_HANDLE);
  EXPECT_FALSE(b.EyeBindingComplete());
}

TEST(OpenXrBackendLifecycle, FutureRecenterPreservesEarlierFrameAndLateChangeDropsIt) {
  OpenXrBackend b;
  Access::Configure(b);
  Access::FrameAt(b, 100);
  Access::ReferenceChange(b, 200);
  EXPECT_TRUE(b.PublishedHeadPose().valid);
  Access::ReferenceChange(b, 100);
  EXPECT_FALSE(b.PublishedHeadPose().valid);
  Access::FrameAt(b, 300);
  Access::ReferenceChange(b, 200);
  EXPECT_FALSE(b.PublishedHeadPose().valid);
}

TEST(OpenXrBackendLifecycle, SynchronizedVisibilityTransitionUsesActualStateHandler) {
  OpenXrBackend b;
  Access::Configure(b);
  Access::State(b, XR_SESSION_STATE_VISIBLE);
  Access::State(b, XR_SESSION_STATE_SYNCHRONIZED);
  EXPECT_TRUE(Access::VisibilityLost(b));
  EXPECT_FALSE(b.PublishedHeadPose().valid);
  Access::FrameAt(b, 100);
  Access::State(b, XR_SESSION_STATE_VISIBLE);
  EXPECT_FALSE(Access::VisibilityLost(b));
  EXPECT_FALSE(b.PublishedHeadPose().valid);
}

TEST(OpenXrBackendLifecycle, SessionLossKeepsLiveDeviceProvenanceButDeviceDestructionCancelsRecovery) {
  OpenXrBackend b;
  Access::Configure(b);
  Access::SeedResource(b);
  Access::State(b, XR_SESSION_STATE_LOSS_PENDING);
  EXPECT_TRUE(Access::Recovering(b));
  EXPECT_EQ(Access::Device(b), Access::Handle<VkDevice>(2));
  EXPECT_EQ(Access::Resources(b), 1u);
  EXPECT_TRUE(b.WantsResourceRecords());
  EXPECT_FALSE(b.PublishedHeadPose().valid);
  b.NoteDestroyImage(Access::Handle<VkImage>(99));
  EXPECT_EQ(Access::Resources(b), 0u);
  b.NoteDeviceDestroyed(Access::Handle<VkDevice>(2));
  EXPECT_FALSE(Access::Recovering(b));
  EXPECT_EQ(Access::Device(b), VK_NULL_HANDLE);
}

TEST(OpenXrBackendLifecycle, InvalidationDropsPoseAndAppliedRecord) {
  OpenXrBackend b;
  Access::Configure(b);
  Access::SetFrameOpen(b, true);
  auto* owner = Access::Handle<void*>(100);
  Access::NotePose(b, owner);
  ASSERT_EQ(Access::AppliedPoses(b), 1u);
  ASSERT_TRUE(b.PublishedHeadPose().valid);
  // A reference-space change / visibility transition must drop the pose and
  // the per-owner application record so the frame cycle cannot retag the
  // in-flight images with a pose from the previous origin.
  Access::Invalidate(b);
  EXPECT_FALSE(b.PublishedHeadPose().valid);
  EXPECT_EQ(Access::AppliedPoses(b), 0u);
}

TEST(OpenXrBackendLifecycle, VisibilityLostStateSurvivesUntilSessionReset) {
  OpenXrBackend b;
  Access::Configure(b);
  Access::SetVisibilityLost(b, true);
  EXPECT_TRUE(Access::VisibilityLost(b));
  // Teardown (device destroyed) resets the flag so a recreated session starts
  // visible; it must not inherit IDLE state from the previous session.
  b.NoteDeviceDestroyed(Access::Handle<VkDevice>(2));
  EXPECT_FALSE(Access::VisibilityLost(b));
}

TEST(OpenXrBackendLifecycle, TeardownClearsProjectionRejectionLatch) {
  OpenXrBackend b;
  Access::Configure(b);
  Access::LatchProjectionRejection(b);
  ASSERT_TRUE(Access::ProjectionRejectionLogged(b));
  b.NoteDeviceDestroyed(Access::Handle<VkDevice>(2));
  EXPECT_FALSE(Access::ProjectionRejectionLogged(b));
}
}  // namespace mocktail::vr

namespace mocktail::vr {
TEST(OpenXrDesktopMirror, RequiresTransferUsageAndDropsDestroyedSwapchain) {
  OpenXrBackend b;
  OpenXrBackendTestAccess::Configure(b);
  const auto device = OpenXrBackendTestAccess::Handle<VkDevice>(2);
  const auto chain = OpenXrBackendTestAccess::Handle<VkSwapchainKHR>(20);
  const auto image = OpenXrBackendTestAccess::Handle<VkImage>(21);
  VkSwapchainCreateInfoKHR info{};
  info.imageArrayLayers = 1;
  info.imageExtent = {800, 600};
  info.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
  b.RecordDesktopSwapchain(device, chain, &info, &image, 1);
  EXPECT_EQ(OpenXrBackendTestAccess::DesktopCount(b), 0u);
  info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  b.RecordDesktopSwapchain(device, chain, &info, &image, 1);
  EXPECT_EQ(OpenXrBackendTestAccess::DesktopCount(b), 1u);
  b.RecordDesktopSwapchain(device, chain, nullptr, nullptr, 0);
  EXPECT_EQ(OpenXrBackendTestAccess::DesktopCount(b), 0u);
  b.RecordDesktopSwapchain(OpenXrBackendTestAccess::Handle<VkDevice>(3), chain, &info, &image, 1);
  EXPECT_EQ(OpenXrBackendTestAccess::DesktopCount(b), 0u);
  EXPECT_EQ(b.MirrorDesktop(VK_NULL_HANDLE, nullptr), VK_NOT_READY);
}
}

namespace mocktail::vr {
namespace {
VkResult reset_result, submit_result, wait_result;
int resets, submits, waits;
VkQueue expected_queue;
VkSemaphore expected_semaphores[2];
VKAPI_ATTR VkResult VKAPI_CALL ResetPresentFence(VkDevice, uint32_t count,
                                                 const VkFence*) {
  EXPECT_EQ(count, 1u);
  ++resets;
  return reset_result;
}
VKAPI_ATTR VkResult VKAPI_CALL SubmitPresentWait(VkQueue queue, uint32_t count,
                                                 const VkSubmitInfo* info,
                                                 VkFence fence) {
  ++submits;
  EXPECT_EQ(queue, expected_queue);
  EXPECT_EQ(count, 1u);
  EXPECT_NE(fence, VK_NULL_HANDLE);
  EXPECT_EQ(info->commandBufferCount, 0u);
  EXPECT_EQ(info->waitSemaphoreCount, 2u);
  EXPECT_EQ(info->signalSemaphoreCount, 0u);
  for (unsigned i = 0; i < 2; ++i) {
    EXPECT_EQ(info->pWaitSemaphores[i], expected_semaphores[i]);
    EXPECT_EQ(info->pWaitDstStageMask[i], VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
  }
  return submit_result;
}
VKAPI_ATTR VkResult VKAPI_CALL WaitPresentFence(VkDevice, uint32_t count,
                                                const VkFence*, VkBool32 all,
                                                uint64_t timeout) {
  ++waits;
  EXPECT_EQ(count, 1u);
  EXPECT_EQ(all, VK_TRUE);
  EXPECT_EQ(timeout, UINT64_MAX);
  return wait_result;
}
class OpenXrPresentDependencies : public testing::Test {
 protected:
  VkPresentInfoKHR info{};
  void SetUp() override {
    reset_result = submit_result = wait_result = VK_SUCCESS;
    resets = submits = waits = 0;
    expected_queue = Access::Handle<VkQueue>(30);
    expected_semaphores[0] = Access::Handle<VkSemaphore>(31);
    expected_semaphores[1] = Access::Handle<VkSemaphore>(32);
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 2;
    info.pWaitSemaphores = expected_semaphores;
  }
  VkResult Run() {
    return internal::WaitForPresentDependencies(
        Access::Handle<VkDevice>(2), expected_queue, info,
        Access::Handle<VkFence>(33), ResetPresentFence, SubmitPresentWait,
        WaitPresentFence);
  }
};
TEST_F(OpenXrPresentDependencies, ConsumesDependenciesBeforePermittingCopies) {
  EXPECT_EQ(Run(), VK_SUCCESS);
  EXPECT_EQ(resets, 1);
  EXPECT_EQ(submits, 1);
  EXPECT_EQ(waits, 1);
  EXPECT_EQ(info.waitSemaphoreCount, 2u);
}
TEST_F(OpenXrPresentDependencies, MirrorConsumedWaitsNeedNoSecondSubmission) {
  info.waitSemaphoreCount = 0;
  info.pWaitSemaphores = nullptr;
  EXPECT_EQ(Run(), VK_SUCCESS);
  EXPECT_EQ(resets + submits + waits, 0);
}
TEST_F(OpenXrPresentDependencies, SubmitFailureDoesNotWaitForUnsignalledFence) {
  submit_result = VK_ERROR_DEVICE_LOST;
  EXPECT_EQ(Run(), VK_ERROR_DEVICE_LOST);
  EXPECT_EQ(submits, 1);
  EXPECT_EQ(waits, 0);
}
TEST_F(OpenXrPresentDependencies,
       ConsumedWaitsNeverAuthorizeFallbackOnTimeout) {
  wait_result = VK_TIMEOUT;
  EXPECT_LT(Run(), VK_SUCCESS);
  EXPECT_EQ(submits, 1);
  EXPECT_EQ(waits, 1);
}
TEST_F(OpenXrPresentDependencies, InvalidDependenciesDoNotSubmit) {
  info.pWaitSemaphores = nullptr;
  EXPECT_LT(Run(), VK_SUCCESS);
  EXPECT_EQ(resets + submits + waits, 0);
}
TEST(OpenXrPresentQueue, RoutesSameAndSplitFamilyPresentationToGraphicsQueue) {
  OpenXrBackend backend;
  Access::Configure(backend);
  Access::GraphicsFamily(backend, 3);
  const auto device = Access::Handle<VkDevice>(2);
  const auto graphics = Access::Handle<VkQueue>(30);
  const auto same_family = Access::Handle<VkQueue>(31);
  const auto split_family = Access::Handle<VkQueue>(32);
  backend.NoteQueue(device, graphics, 3, 0);
  backend.NoteQueue(device, same_family, 3, 1);
  backend.NoteQueue(device, split_family, 4, 0);
  EXPECT_TRUE(Access::SupportsQueue(backend, graphics));
  EXPECT_TRUE(Access::SupportsQueue(backend, same_family));
  EXPECT_TRUE(Access::SupportsQueue(backend, split_family));
  EXPECT_EQ(Access::CopyQueue(backend, split_family), graphics);
  EXPECT_FALSE(Access::SupportsQueue(backend, Access::Handle<VkQueue>(99)));
}
}  // namespace
}  // namespace mocktail::vr

namespace mocktail::vr {
TEST(OpenXrBackendBinding,
     SelectsSingleSampleResolveAlongsideMultisampleColor) {
  OpenXrBackend backend;
  RobloxVrDeviceBridge bridge;
  internal::VrBridgeTestAccess::Enable(bridge);
  Access::Configure(backend);
  auto* owner = Access::Handle<void*>(100);
  const auto device = Access::Handle<VkDevice>(2);
  Access::Attachment(backend, 10, owner, device);
  Access::Attachment(backend, 11, owner, device);
  Access::AddMsaaAttachment(backend, 10, 11);
  Access::ResolvedPass(backend, bridge, owner, 10);
  EXPECT_EQ(Access::Image(backend, 0), Access::Handle<VkImage>(10));
  EXPECT_EQ(Access::Layout(backend, 0),
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}
TEST(OpenXrBackendBinding, RejectsMultisampleImageWithoutResolve) {
  OpenXrBackend backend;
  RobloxVrDeviceBridge bridge;
  internal::VrBridgeTestAccess::Enable(bridge);
  Access::Configure(backend);
  auto* owner = Access::Handle<void*>(100);
  Access::Attachment(backend, 10, owner, Access::Handle<VkDevice>(2));
  Access::AddMsaaAttachment(backend, 10, 10);
  Access::ResolvedPass(backend, bridge, owner, 10);
  EXPECT_EQ(Access::Image(backend, 0), VK_NULL_HANDLE);
}
}  // namespace mocktail::vr

namespace mocktail::vr {
namespace {
class OpenXrSplitPresentation : public testing::Test {
 protected:
  OpenXrBackend backend;
  RobloxVrDeviceBridge bridge;
  const VkDevice device = Access::Handle<VkDevice>(2);
  const VkQueue graphics = Access::Handle<VkQueue>(30);
  const VkQueue present = Access::Handle<VkQueue>(31);
  const VkQueue graphics_second = Access::Handle<VkQueue>(32);
  const VkCommandPool graphics_pool = Access::Handle<VkCommandPool>(40);
  const VkCommandPool present_pool = Access::Handle<VkCommandPool>(41);
  const VkCommandBuffer draw = Access::Handle<VkCommandBuffer>(50);
  const VkCommandBuffer acquire = Access::Handle<VkCommandBuffer>(51);
  const VkSwapchainKHR chain = Access::Handle<VkSwapchainKHR>(60);
  const VkImage desktop = Access::Handle<VkImage>(61);
  void* const owner = Access::Handle<void*>(100);
  void SetUp() override {
    Access::Configure(backend);
    Access::GraphicsFamily(backend, 3);
    internal::VrBridgeTestAccess::Enable(bridge);
    backend.NoteQueue(device, graphics, 3, 0);
    backend.NoteQueue(device, present, 4, 0);
    backend.NoteQueue(device, graphics_second, 3, 1);
    backend.RecordCommandPool(device, graphics_pool, 3);
    backend.RecordCommandPool(device, present_pool, 4);
    backend.RecordCommandBuffers(device, graphics_pool, 1, &draw);
    backend.RecordCommandBuffers(device, present_pool, 1, &acquire);
    RegisterDesktop(true);
  }
  void RegisterDesktop(bool converted) {
    const unsigned families[] = {3, 4};
    VkSwapchainCreateInfoKHR info{};
    info.imageArrayLayers = 1;
    info.imageExtent = {800, 600};
    info.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    info.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
    info.queueFamilyIndexCount = 2;
    info.pQueueFamilyIndices = families;
    backend.RecordDesktopSwapchain(device, chain, &info, &desktop, 1,
                                   converted);
  }
  void Draw() {
    Access::Attachment(backend, 10, owner, device);
    Access::Pass(backend, bridge, 0, 10, owner,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, draw);
  }
};
TEST_F(OpenXrSplitPresentation, ActualSubmittedGraphicsQueueOwnsTheEyeCopy) {
  Draw();
  EXPECT_EQ(Access::CopyQueue(backend, present), VK_NULL_HANDLE);
  backend.NoteCommandBuffersSubmitted(present, 1, &draw);  // Wrong pool family.
  EXPECT_EQ(Access::CopyQueue(backend, present), VK_NULL_HANDLE);
  backend.NoteCommandBuffersSubmitted(graphics_second, 1, &draw);
  EXPECT_EQ(Access::CopyQueue(backend, present), graphics_second);
  Access::NextFrame(backend);
  backend.ResetCommandBuffer(draw);
  Draw();
  EXPECT_EQ(Access::CopyQueue(backend, present), VK_NULL_HANDLE);
  backend.NoteCommandBuffersSubmitted(graphics, 1, &draw);
  EXPECT_EQ(Access::CopyQueue(backend, present), graphics);
}
TEST_F(OpenXrSplitPresentation,
       ResetAndFreeDoNotResurrectUnsubmittedEyeCommands) {
  Draw();
  backend.ResetCommandBuffer(draw);
  backend.NoteCommandBuffersSubmitted(graphics, 1, &draw);
  EXPECT_EQ(Access::CopyQueue(backend, present), VK_NULL_HANDLE);
  Draw();
  backend.ForgetCommandPool(graphics_pool);
  backend.NoteCommandBuffersSubmitted(graphics, 1, &draw);
  EXPECT_EQ(Access::CopyQueue(backend, present), VK_NULL_HANDLE);
}
TEST_F(OpenXrSplitPresentation, ConcurrentSwapchainTracksBothFamilies) {
  EXPECT_EQ(Access::DesktopCount(backend), 1u);
  EXPECT_EQ(backend.DesktopQueueFamilies(device),
            (std::vector<unsigned>{3, 4}));
  EXPECT_TRUE(
      backend.DesktopQueueFamilies(Access::Handle<VkDevice>(9)).empty());
}
TEST_F(OpenXrSplitPresentation, PoolResetAndReusedImageRequireNewSubmission) {
  Draw();
  backend.NoteCommandBuffersSubmitted(graphics, 1, &draw);
  ASSERT_EQ(Access::CopyQueue(backend, present), graphics);
  backend.ResetCommandPool(graphics_pool);
  EXPECT_EQ(Access::CopyQueue(backend, present), VK_NULL_HANDLE);
  Draw();
  backend.NoteCommandBuffersSubmitted(graphics, 1, &draw);
  ASSERT_EQ(Access::CopyQueue(backend, present), graphics);
  backend.NoteDestroyImage(Access::Handle<VkImage>(10));
  Draw();
  EXPECT_EQ(Access::CopyQueue(backend, present), VK_NULL_HANDLE);
  backend.NoteCommandBuffersSubmitted(graphics_second, 1, &draw);
  EXPECT_EQ(Access::CopyQueue(backend, present), graphics_second);
}
TEST_F(OpenXrSplitPresentation,
       ReleaseTransitionsOnceAndMatchingAcquireDoesNotRepeat) {
  unsigned source = 3, destination = 4;
  VkImageLayout old_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  backend.NormalizeDesktopBarrier(draw, desktop, &source, &destination,
                                  &old_layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  EXPECT_EQ(source, VK_QUEUE_FAMILY_IGNORED);
  EXPECT_EQ(destination, VK_QUEUE_FAMILY_IGNORED);
  EXPECT_EQ(old_layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  source = 3;
  destination = 4;
  backend.NormalizeDesktopBarrier(acquire, desktop, &source, &destination,
                                  &old_layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  EXPECT_EQ(source, VK_QUEUE_FAMILY_IGNORED);
  EXPECT_EQ(destination, VK_QUEUE_FAMILY_IGNORED);
  EXPECT_EQ(old_layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
}
TEST_F(OpenXrSplitPresentation, WsiAcquirePreservesPresentToColorTransition) {
  unsigned source = 4, destination = 3;
  VkImageLayout old_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  backend.NormalizeDesktopBarrier(draw, desktop, &source, &destination,
                                  &old_layout,
                                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  EXPECT_EQ(source, VK_QUEUE_FAMILY_IGNORED);
  EXPECT_EQ(destination, VK_QUEUE_FAMILY_IGNORED);
  EXPECT_EQ(old_layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  source = 4;
  destination = 3;
  old_layout = VK_IMAGE_LAYOUT_UNDEFINED;
  backend.NormalizeDesktopBarrier(draw, desktop, &source, &destination,
                                  &old_layout,
                                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  EXPECT_EQ(old_layout, VK_IMAGE_LAYOUT_UNDEFINED);
}
TEST_F(OpenXrSplitPresentation,
       ExplicitPresentReleaseDefersLayoutTransitionToAcquire) {
  unsigned source = 4, destination = 3;
  VkImageLayout old_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  const auto released_layout = backend.NormalizeDesktopBarrier(
      acquire, desktop, &source, &destination, &old_layout,
      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  EXPECT_EQ(old_layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  EXPECT_EQ(released_layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  EXPECT_EQ(source, VK_QUEUE_FAMILY_IGNORED);
  source = 4;
  destination = 3;
  const auto acquired_layout = backend.NormalizeDesktopBarrier(
      draw, desktop, &source, &destination, &old_layout,
      VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  EXPECT_EQ(old_layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  EXPECT_EQ(acquired_layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
}
TEST_F(OpenXrSplitPresentation,
       OriginalConcurrentAndUnrelatedImagesRemainUnchanged) {
  RegisterDesktop(false);
  unsigned source = 3, destination = 4;
  VkImageLayout old_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  backend.NormalizeDesktopBarrier(acquire, desktop, &source, &destination,
                                  &old_layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  EXPECT_EQ(source, 3u);
  EXPECT_EQ(destination, 4u);
  RegisterDesktop(true);
  backend.NormalizeDesktopBarrier(acquire, Access::Handle<VkImage>(99), &source,
                                  &destination, &old_layout,
                                  VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  EXPECT_EQ(source, 3u);
  backend.RecordDesktopSwapchain(device, chain, nullptr, nullptr, 0);
  backend.NormalizeDesktopBarrier(acquire, desktop, &source, &destination,
                                  &old_layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
  EXPECT_EQ(source, 3u);
}
}  // namespace
}  // namespace mocktail::vr
