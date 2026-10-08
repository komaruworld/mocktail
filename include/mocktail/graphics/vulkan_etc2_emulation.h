#ifndef MOCKTAIL_GRAPHICS_VULKAN_ETC2_EMULATION_H_
#define MOCKTAIL_GRAPHICS_VULKAN_ETC2_EMULATION_H_

#include <vulkan/vulkan.h>

#include <cstdint>

#include "mocktail/graphics/etc2_decoder.h"

namespace mocktail::graphics {

struct Etc2EmulatedFormat {
  VkFormat host_format = VK_FORMAT_UNDEFINED;
  EtcFormat etc_format = EtcFormat::kEtc2Rgb8;
};

bool LookupEmulatedEtc2Format(VkFormat format, Etc2EmulatedFormat* out);

VkFormatFeatureFlags EmulatedEtc2FormatFeatures(
    VkFormatFeatureFlags host_features);

bool Etc2UploadByteCounts(EtcFormat format, const VkExtent3D& extent,
                          std::uint32_t layer_count, VkDeviceSize* compressed,
                          VkDeviceSize* decoded);

bool Etc2SupportAdvertised(const char* value);
bool Etc2SupportAdvertised();

inline constexpr std::uint32_t kSmallTextureMaxExtent = 64;
inline constexpr std::uint32_t kDefaultSmallTextureUpscale = 4;
std::uint32_t SmallTextureUpscale(const char* value);

class VulkanEtc2Emulation final {
 public:
  VulkanEtc2Emulation();
  ~VulkanEtc2Emulation();
  VulkanEtc2Emulation(const VulkanEtc2Emulation&) = delete;
  VulkanEtc2Emulation& operator=(const VulkanEtc2Emulation&) = delete;

  bool PhysicalDeviceNeedsEmulation(VkPhysicalDevice physical_device,
                                    PFN_vkGetPhysicalDeviceFeatures host);

  void RegisterDevice(VkDevice device, VkPhysicalDevice physical_device,
                      bool emulated,
                      const VkPhysicalDeviceMemoryProperties& memory,
                      PFN_vkGetDeviceProcAddr get_device_proc_addr);
  void DestroyDevice(VkDevice device);

  VkResult CreateImage(VkDevice device, const VkImageCreateInfo* create_info,
                       const VkAllocationCallbacks* allocator, VkImage* image);
  void DestroyImage(VkDevice device, VkImage image,
                    const VkAllocationCallbacks* allocator);
  VkResult CreateImageView(VkDevice device,
                           const VkImageViewCreateInfo* create_info,
                           const VkAllocationCallbacks* allocator,
                           VkImageView* view);
  VkResult BindBufferMemory(VkDevice device, VkBuffer buffer,
                            VkDeviceMemory memory, VkDeviceSize offset);
  VkResult BindBufferMemory2(VkDevice device, std::uint32_t count,
                             const VkBindBufferMemoryInfo* infos);
  VkResult MapMemory(VkDevice device, VkDeviceMemory memory,
                     VkDeviceSize offset, VkDeviceSize size,
                     VkMemoryMapFlags flags, void** data);
  VkResult MapMemory2(VkDevice device, const VkMemoryMapInfo* info,
                      void** data);
  void UnmapMemory(VkDevice device, VkDeviceMemory memory);
  VkResult UnmapMemory2(VkDevice device, const VkMemoryUnmapInfo* info);
  void FreeMemory(VkDevice device, VkDeviceMemory memory,
                  const VkAllocationCallbacks* allocator);
  void DestroyBuffer(VkDevice device, VkBuffer buffer,
                     const VkAllocationCallbacks* allocator);
  void CmdCopyBufferToImage(VkDevice device, VkCommandBuffer command_buffer,
                            VkBuffer source, VkImage destination,
                            VkImageLayout layout, std::uint32_t region_count,
                            const VkBufferImageCopy* regions);
  void CmdCopyBufferToImage2(VkDevice device, VkCommandBuffer command_buffer,
                             const VkCopyBufferToImageInfo2* info);
  void CmdCopyImage(VkDevice device, VkCommandBuffer command_buffer,
                    VkImage source, VkImageLayout source_layout,
                    VkImage destination, VkImageLayout destination_layout,
                    std::uint32_t region_count, const VkImageCopy* regions);
  void CmdCopyImage2(VkDevice device, VkCommandBuffer command_buffer,
                     const VkCopyImageInfo2* info);
  void CmdExecuteCommands(VkDevice device, VkCommandBuffer command_buffer,
                          std::uint32_t count,
                          const VkCommandBuffer* secondaries);

  // Resubmission must reread the source bytes.
  bool HasPendingUploads() const;
  void PrepareSubmit(const VkCommandBuffer* command_buffers,
                     std::uint32_t count);
  // Call only after the command buffer is no longer pending.
  void ReleaseCommandBuffer(VkCommandBuffer command_buffer);

  struct State;

 private:
  State* state_;
};

}  // namespace mocktail::graphics

#endif  // MOCKTAIL_GRAPHICS_VULKAN_ETC2_EMULATION_H_
