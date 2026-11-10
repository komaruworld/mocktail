#include "mocktail/graphics/vulkan_swapchain_policy.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>

namespace mocktail::graphics {
namespace {

struct HostSurface {
  std::vector<VkPresentModeKHR> modes = {
      VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_MAILBOX_KHR,
      VK_PRESENT_MODE_FIFO_KHR};
  std::uint32_t mailbox_min = 4;
  std::uint32_t maximum = 0;
  VkResult result = VK_SUCCESS;
  VkPresentModeKHR queried_mode = VK_PRESENT_MODE_MAX_ENUM_KHR;
  int legacy_queries = 0;
  int mode_queries = 0;
} host;

VkResult VKAPI_CALL PresentModes(VkPhysicalDevice, VkSurfaceKHR,
                                 std::uint32_t* count, VkPresentModeKHR* modes) {
  if (host.result != VK_SUCCESS) return host.result;
  if (modes == nullptr) {
    *count = host.modes.size();
    return VK_SUCCESS;
  }
  *count = std::min<std::size_t>(*count, host.modes.size());
  std::copy_n(host.modes.begin(), *count, modes);
  return *count < host.modes.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

VkResult VKAPI_CALL Capabilities(VkPhysicalDevice, VkSurfaceKHR,
                                 VkSurfaceCapabilitiesKHR* caps) {
  ++host.legacy_queries;
  caps->minImageCount = 3;
  caps->maxImageCount = host.maximum;
  return host.result;
}

VkResult VKAPI_CALL ModeCapabilities(
    VkPhysicalDevice, const VkPhysicalDeviceSurfaceInfo2KHR* info,
    VkSurfaceCapabilities2KHR* caps) {
  EXPECT_EQ(info->sType, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR);
  EXPECT_EQ(caps->sType, VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR);
  const auto* mode = static_cast<const VkSurfacePresentModeKHR*>(info->pNext);
  EXPECT_NE(mode, nullptr);
  if (mode == nullptr) return VK_ERROR_INITIALIZATION_FAILED;
  EXPECT_EQ(mode->sType, VK_STRUCTURE_TYPE_SURFACE_PRESENT_MODE_KHR);
  host.queried_mode = mode->presentMode;
  ++host.mode_queries;
  caps->surfaceCapabilities.minImageCount =
      mode->presentMode == VK_PRESENT_MODE_MAILBOX_KHR ? host.mailbox_min : 3;
  caps->surfaceCapabilities.maxImageCount = host.maximum;
  return host.result;
}

class VulkanSwapchainPolicyTest : public ::testing::Test {
 protected:
  void SetUp() override {
    host = {};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.minImageCount = 3;
    info.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;
    info.imageExtent = {1280, 720};
    info.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.clipped = VK_TRUE;
  }
  VkResult Apply(PresentModePolicy policy) {
    return ApplySwapchainPresentPolicy(
        policy, reinterpret_cast<VkPhysicalDevice>(std::uintptr_t{1}), queries,
        &info);
  }
  VulkanSurfaceQueries queries{PresentModes, Capabilities, ModeCapabilities};
  VkSwapchainCreateInfoKHR info{};
};

TEST_F(VulkanSwapchainPolicyTest, WaylandAutoUsesMailboxWithModeSpecificHeadroom) {
  const auto policy = ResolveVulkanSurfacePolicy(
      ResolvePresentModePolicy("auto", "-1"), true);
  ASSERT_EQ(Apply(policy), VK_SUCCESS);
  EXPECT_EQ(info.presentMode, VK_PRESENT_MODE_MAILBOX_KHR);
  EXPECT_EQ(host.queried_mode, VK_PRESENT_MODE_MAILBOX_KHR);
  EXPECT_EQ(host.mode_queries, 1);
  EXPECT_EQ(host.legacy_queries, 0);
  EXPECT_EQ(info.minImageCount, 5U);
  // Even if the presentation engine retains its minimum, the application
  // can acquire another image without waiting for a release.
  EXPECT_GT(info.minImageCount, host.mailbox_min);
  EXPECT_EQ(info.imageExtent.width, 1280U);
  EXPECT_EQ(info.imageFormat, VK_FORMAT_B8G8R8A8_UNORM);
  EXPECT_EQ(info.imageUsage, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
  EXPECT_EQ(info.clipped, VK_TRUE);
}

TEST_F(VulkanSwapchainPolicyTest, ExplicitImmediateAndFifoArePreserved) {
  for (const char* mode : {"immediate", "fifo"}) {
    const auto policy = ResolveVulkanSurfacePolicy(
        ResolvePresentModePolicy("auto", "-1", mode), true);
    ASSERT_EQ(Apply(policy), VK_SUCCESS);
    EXPECT_EQ(info.presentMode, *ExplicitPresentMode(policy));
    EXPECT_EQ(host.queried_mode, info.presentMode);
    EXPECT_EQ(info.minImageCount, 3U);
  }
}

TEST_F(VulkanSwapchainPolicyTest, QueriesTheActualFallbackMode) {
  host.modes = {VK_PRESENT_MODE_FIFO_KHR};
  ASSERT_EQ(Apply(PresentModePolicy::kMailbox), VK_SUCCESS);
  EXPECT_EQ(info.presentMode, VK_PRESENT_MODE_FIFO_KHR);
  EXPECT_EQ(host.queried_mode, VK_PRESENT_MODE_FIFO_KHR);
  EXPECT_EQ(info.minImageCount, 3U);
}

TEST_F(VulkanSwapchainPolicyTest, SupportsHostsWithoutSurfaceMaintenance) {
  queries.mode_capabilities = nullptr;
  ASSERT_EQ(Apply(PresentModePolicy::kMailbox), VK_SUCCESS);
  EXPECT_EQ(host.mode_queries, 0);
  EXPECT_EQ(host.legacy_queries, 1);
  EXPECT_EQ(info.minImageCount, 4U);
}

TEST_F(VulkanSwapchainPolicyTest, RespectsImageLimitsAndLargerGuestRequests) {
  host.maximum = 4;
  ASSERT_EQ(Apply(PresentModePolicy::kMailbox), VK_SUCCESS);
  EXPECT_EQ(info.minImageCount, 4U);
  host.maximum = 0;
  info.minImageCount = 7;
  ASSERT_EQ(Apply(PresentModePolicy::kMailbox), VK_SUCCESS);
  EXPECT_EQ(info.minImageCount, 7U);
}

TEST_F(VulkanSwapchainPolicyTest, DoesNotPartiallyRewriteOnQueryFailure) {
  const auto original = info;
  host.result = VK_ERROR_SURFACE_LOST_KHR;
  EXPECT_EQ(Apply(PresentModePolicy::kMailbox), VK_ERROR_SURFACE_LOST_KHR);
  EXPECT_EQ(info.presentMode, original.presentMode);
  EXPECT_EQ(info.minImageCount, original.minImageCount);
  host.result = VK_SUCCESS;
  host.modes.clear();
  EXPECT_EQ(Apply(PresentModePolicy::kMailbox), VK_ERROR_INITIALIZATION_FAILED);
  EXPECT_EQ(info.presentMode, original.presentMode);
}

std::vector<VkExtensionProperties> Extensions(
    std::initializer_list<const char*> names) {
  std::vector<VkExtensionProperties> extensions;
  for (const char* name : names) {
    VkExtensionProperties extension{};
    std::strncpy(extension.extensionName, name,
                  sizeof(extension.extensionName) - 1);
    extensions.push_back(extension);
  }
  return extensions;
}

TEST(SurfaceMaintenanceTest, EnablesSupportedExtensionAndDependenciesOnce) {
  for (const char* name : {VK_KHR_SURFACE_MAINTENANCE_1_EXTENSION_NAME,
                            VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME}) {
    auto supported = Extensions({VK_KHR_SURFACE_EXTENSION_NAME,
        VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, name});
    std::vector<std::string> enabled = {VK_KHR_SURFACE_EXTENSION_NAME,
                                      "VK_KHR_wayland_surface"};
    EXPECT_TRUE(EnableSurfaceMaintenanceExtensions(supported, &enabled));
    EXPECT_TRUE(EnableSurfaceMaintenanceExtensions(supported, &enabled));
    EXPECT_EQ(enabled.size(), 4U);
    EXPECT_EQ(std::count(enabled.begin(), enabled.end(), name), 1);
  }
}

TEST(SurfaceMaintenanceTest, DoesNotEnableUnsupportedExtensions) {
  auto supported = Extensions({VK_KHR_SURFACE_EXTENSION_NAME,
                                VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME});
  std::vector<std::string> enabled = {VK_KHR_SURFACE_EXTENSION_NAME};
  EXPECT_FALSE(EnableSurfaceMaintenanceExtensions(supported, &enabled));
  EXPECT_EQ(enabled, (std::vector<std::string>{VK_KHR_SURFACE_EXTENSION_NAME}));
  EXPECT_FALSE(EnableSurfaceMaintenanceExtensions({}, &enabled));
}

}  // namespace
}  // namespace mocktail::graphics
