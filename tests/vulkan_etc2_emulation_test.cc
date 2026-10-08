#include "mocktail/graphics/vulkan_etc2_emulation.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "mocktail/graphics/texture_override.h"

namespace mocktail::graphics {
namespace {

template <typename T>
T Handle(std::uintptr_t value) {
  if constexpr (std::is_pointer_v<T>)
    return reinterpret_cast<T>(value);
  else
    return static_cast<T>(value);
}

class VulkanEtc2Test : public ::testing::TestWithParam<bool> {
 protected:
  void SetUp() override {
    active = this;
    unsetenv("MOCKTAIL_TEXTURE_OVERRIDE_DIR");
    unsetenv("MOCKTAIL_TEXTURE_DUMP_DIR");
    memory_properties.memoryTypeCount = GetParam() ? 2 : 1;
    memory_properties.memoryTypes[0].propertyFlags =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    memory_properties.memoryTypes[1].propertyFlags =
        memory_properties.memoryTypes[0].propertyFlags |
        VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    Start(true, "1");
  }

  void TearDown() override {
    emulation->DestroyDevice(device);
    emulation.reset();
    active = nullptr;
  }

  void Start(bool emulated, const char* upscale) {
    if (emulation) emulation->DestroyDevice(device);
    setenv("MOCKTAIL_SMALL_TEXTURE_UPSCALE", upscale, 1);
    emulation = std::make_unique<VulkanEtc2Emulation>();
    emulation->RegisterDevice(device, Handle<VkPhysicalDevice>(2), emulated,
                              memory_properties, GetProc);
  }

  struct Source {
    VkBuffer buffer;
    VkDeviceMemory memory;
    std::uint8_t* data;
  };

  Source CreateSource(std::size_t bytes, bool mapped = true) {
    const VkBuffer buffer = Handle<VkBuffer>(next++);
    const VkDeviceMemory memory = Handle<VkDeviceMemory>(next++);
    sizes[buffer] = bytes;
    memory_bytes[memory].resize(bytes);
    EXPECT_EQ(emulation->BindBufferMemory(device, buffer, memory, 0),
              VK_SUCCESS);
    void* data = memory_bytes[memory].data();
    if (mapped) {
      EXPECT_EQ(emulation->MapMemory(device, memory, 0, bytes, 0, &data),
                VK_SUCCESS);
    }
    return {buffer, memory, static_cast<std::uint8_t*>(data)};
  }

  VkImage CreateImage(std::uint32_t width, std::uint32_t height,
                      std::uint32_t layers = 1) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK;
    info.extent = {width, height, 1};
    info.arrayLayers = layers;
    info.mipLevels = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    VkImage image = VK_NULL_HANDLE;
    EXPECT_EQ(emulation->CreateImage(device, &info, nullptr, &image),
              VK_SUCCESS);
    return image;
  }

  void Record(VkCommandBuffer command, const Source& source, VkImage image,
              std::uint32_t width, std::uint32_t height,
              std::uint32_t layers = 1, VkDeviceSize offset = 0,
              std::uint32_t row_length = 0, std::uint32_t image_height = 0) {
    VkBufferImageCopy region{};
    region.bufferOffset = offset;
    region.bufferRowLength = row_length;
    region.bufferImageHeight = image_height;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, layers};
    region.imageExtent = {width, height, 1};
    emulation->CmdCopyBufferToImage(device, command, source.buffer, image,
                                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                    &region);
  }

  const std::vector<std::uint8_t>& Output(VkCommandBuffer command) const {
    return memory_bytes.at(bindings.at(recorded.at(command)).first);
  }

  static std::vector<std::uint8_t> Decode(const std::uint8_t* source,
                                          std::uint32_t width,
                                          std::uint32_t height) {
    std::vector<std::uint8_t> result(width * height * 4);
    EXPECT_TRUE(DecodeEtcImage(EtcFormat::kEtc2Rgba8, source,
                               ((width + 3) / 4) * ((height + 3) / 4) * 16,
                               width, height, result.data(), result.size()));
    return result;
  }

  static VkResult VKAPI_CALL HostCreateImage(VkDevice,
                                             const VkImageCreateInfo* info,
                                             const VkAllocationCallbacks*,
                                             VkImage* image) {
    active->last_image = *info;
    *image = Handle<VkImage>(active->next++);
    return VK_SUCCESS;
  }

  static void VKAPI_CALL HostDestroyImage(VkDevice, VkImage,
                                          const VkAllocationCallbacks*) {}

  static VkResult VKAPI_CALL HostCreateBuffer(VkDevice,
                                              const VkBufferCreateInfo* info,
                                              const VkAllocationCallbacks*,
                                              VkBuffer* buffer) {
    *buffer = Handle<VkBuffer>(active->next++);
    active->sizes[*buffer] = info->size;
    return VK_SUCCESS;
  }

  static void VKAPI_CALL HostRequirements(VkDevice, VkBuffer buffer,
                                          VkMemoryRequirements* requirements) {
    *requirements = {active->sizes.at(buffer), 16,
                     active->GetParam() ? 3U : 1U};
  }

  static VkResult VKAPI_CALL HostAllocate(VkDevice,
                                          const VkMemoryAllocateInfo* info,
                                          const VkAllocationCallbacks*,
                                          VkDeviceMemory* memory) {
    *memory = Handle<VkDeviceMemory>(active->next++);
    active->memory_bytes[*memory].resize(info->allocationSize, 0xa5);
    active->last_memory_type = info->memoryTypeIndex;
    return VK_SUCCESS;
  }

  static VkResult VKAPI_CALL HostBind(VkDevice, VkBuffer buffer,
                                      VkDeviceMemory memory,
                                      VkDeviceSize offset) {
    active->bindings[buffer] = {memory, offset};
    return VK_SUCCESS;
  }

  static VkResult VKAPI_CALL HostMap(VkDevice, VkDeviceMemory memory,
                                     VkDeviceSize offset, VkDeviceSize,
                                     VkMemoryMapFlags, void** data) {
    ++active->map_calls;
    *data = active->memory_bytes.at(memory).data() + offset;
    return VK_SUCCESS;
  }

  static void VKAPI_CALL HostUnmap(VkDevice, VkDeviceMemory) {
    ++active->unmap_calls;
  }

  static void VKAPI_CALL HostFree(VkDevice, VkDeviceMemory memory,
                                  const VkAllocationCallbacks*) {
    active->memory_bytes.erase(memory);
  }

  static void VKAPI_CALL HostDestroyBuffer(VkDevice, VkBuffer buffer,
                                           const VkAllocationCallbacks*) {
    active->sizes.erase(buffer);
    active->bindings.erase(buffer);
  }

  static void VKAPI_CALL HostCopy(VkCommandBuffer command, VkBuffer buffer,
                                  VkImage, VkImageLayout, std::uint32_t,
                                  const VkBufferImageCopy*) {
    active->recorded[command] = buffer;
  }

  static void VKAPI_CALL HostExecute(VkCommandBuffer, std::uint32_t,
                                     const VkCommandBuffer*) {}

  static PFN_vkVoidFunction VKAPI_CALL GetProc(VkDevice, const char* name) {
    const std::pair<const char*, PFN_vkVoidFunction> functions[] = {
        {"vkCreateImage",
         reinterpret_cast<PFN_vkVoidFunction>(HostCreateImage)},
        {"vkDestroyImage",
         reinterpret_cast<PFN_vkVoidFunction>(HostDestroyImage)},
        {"vkCreateBuffer",
         reinterpret_cast<PFN_vkVoidFunction>(HostCreateBuffer)},
        {"vkGetBufferMemoryRequirements",
         reinterpret_cast<PFN_vkVoidFunction>(HostRequirements)},
        {"vkAllocateMemory",
         reinterpret_cast<PFN_vkVoidFunction>(HostAllocate)},
        {"vkBindBufferMemory", reinterpret_cast<PFN_vkVoidFunction>(HostBind)},
        {"vkMapMemory", reinterpret_cast<PFN_vkVoidFunction>(HostMap)},
        {"vkUnmapMemory", reinterpret_cast<PFN_vkVoidFunction>(HostUnmap)},
        {"vkFreeMemory", reinterpret_cast<PFN_vkVoidFunction>(HostFree)},
        {"vkDestroyBuffer",
         reinterpret_cast<PFN_vkVoidFunction>(HostDestroyBuffer)},
        {"vkCmdCopyBufferToImage",
         reinterpret_cast<PFN_vkVoidFunction>(HostCopy)},
        {"vkCmdExecuteCommands",
         reinterpret_cast<PFN_vkVoidFunction>(HostExecute)},
    };
    for (const auto& entry : functions) {
      if (std::strcmp(name, entry.first) == 0) return entry.second;
    }
    return nullptr;
  }

  inline static VulkanEtc2Test* active = nullptr;
  const VkDevice device = Handle<VkDevice>(1);
  std::uintptr_t next = 100;
  VkPhysicalDeviceMemoryProperties memory_properties{};
  VkImageCreateInfo last_image{};
  std::uint32_t last_memory_type = UINT32_MAX;
  std::atomic<unsigned> map_calls{0};
  std::atomic<unsigned> unmap_calls{0};
  std::unordered_map<VkBuffer, VkDeviceSize> sizes;
  std::unordered_map<VkBuffer, std::pair<VkDeviceMemory, VkDeviceSize>>
      bindings;
  std::unordered_map<VkDeviceMemory, std::vector<std::uint8_t>> memory_bytes;
  std::unordered_map<VkCommandBuffer, VkBuffer> recorded;
  std::unique_ptr<VulkanEtc2Emulation> emulation;
};

TEST_P(VulkanEtc2Test, UsesCurrentSourceOnEverySubmission) {
  auto source = CreateSource(256 * 256);
  const auto image = CreateImage(256, 256);
  const auto command = Handle<VkCommandBuffer>(10);
  Record(command, source, image, 256, 256);
  EXPECT_EQ(last_memory_type, GetParam() ? 1U : 0U);
  for (std::uint8_t value : {0x12, 0x3a}) {
    std::memset(source.data, value, 256 * 256);
    const auto expected = Decode(source.data, 256, 256);
    emulation->PrepareSubmit(&command, 1);
    EXPECT_EQ(Output(command), expected);
  }
  emulation->ReleaseCommandBuffer(command);
  EXPECT_FALSE(emulation->HasPendingUploads());
}

TEST_P(VulkanEtc2Test, DecodesPaddedRowsAndArrayLayers) {
  auto source = CreateSource(16 + 288 * 3 * 2);
  for (unsigned i = 0; i < 16 + 288 * 3 * 2; ++i) source.data[i] = i * 29;
  const auto image = CreateImage(65, 5, 2);
  const auto command = Handle<VkCommandBuffer>(10);
  Record(command, source, image, 65, 5, 2, 16, 72, 12);
  std::vector<std::uint8_t> expected;
  for (unsigned layer = 0; layer < 2; ++layer) {
    std::vector<std::uint8_t> packed(272 * 2);
    for (unsigned row = 0; row < 2; ++row) {
      std::memcpy(packed.data() + row * 272,
                  source.data + 16 + layer * 288 * 3 + row * 288, 272);
    }
    const auto decoded = Decode(packed.data(), 65, 5);
    expected.insert(expected.end(), decoded.begin(), decoded.end());
  }
  emulation->PrepareSubmit(&command, 1);
  EXPECT_EQ(Output(command), expected);
}

TEST_P(VulkanEtc2Test, HandlesConcurrentSubmissionsOfUnmappedSources) {
  auto source = CreateSource(256 * 256, false);
  std::memset(source.data, 0x39, 256 * 256);
  const auto expected = Decode(source.data, 256, 256);
  const auto image = CreateImage(256, 256);
  std::vector<VkCommandBuffer> commands;
  for (unsigned i = 0; i < 4; ++i) {
    commands.push_back(Handle<VkCommandBuffer>(10 + i));
    Record(commands.back(), source, image, 256, 256);
  }
  const unsigned mapped = map_calls.load();
  const unsigned unmapped = unmap_calls.load();
  std::vector<std::thread> callers;
  for (const auto command : commands) {
    callers.emplace_back([&, command] {
      emulation->PrepareSubmit(&command, 1);
      EXPECT_EQ(Output(command), expected);
    });
  }
  for (auto& caller : callers) caller.join();
  EXPECT_EQ(map_calls.load() - mapped, commands.size());
  EXPECT_EQ(unmap_calls.load() - unmapped, commands.size());
}

TEST_P(VulkanEtc2Test, RepeatedSecondaryIsDecodedOnce) {
  auto source = CreateSource(256 * 256, false);
  const auto image = CreateImage(256, 256);
  const auto primary = Handle<VkCommandBuffer>(10);
  const auto secondary = Handle<VkCommandBuffer>(11);
  Record(secondary, source, image, 256, 256);
  const VkCommandBuffer repeated[] = {secondary, secondary};
  emulation->CmdExecuteCommands(device, primary, 2, repeated);
  emulation->PrepareSubmit(&primary, 1);
  EXPECT_EQ(Output(secondary), Decode(source.data, 256, 256));
  emulation->ReleaseCommandBuffer(primary);
  emulation->ReleaseCommandBuffer(secondary);
  EXPECT_FALSE(emulation->HasPendingUploads());
}

TEST_P(VulkanEtc2Test, UpscalingStillProducesTheSamePixels) {
  Start(true, "4");
  auto source = CreateSource(4 * 4 * 16);
  std::memset(source.data, 0x53, 4 * 4 * 16);
  const auto image = CreateImage(16, 16);
  const auto command = Handle<VkCommandBuffer>(10);
  Record(command, source, image, 16, 16);
  std::vector<std::uint8_t> expected(64 * 64 * 4);
  const auto decoded = Decode(source.data, 16, 16);
  ResampleRgba(decoded.data(), 16, 16, 64, 64, expected.data());
  emulation->PrepareSubmit(&command, 1);
  EXPECT_EQ(Output(command), expected);
}

TEST_P(VulkanEtc2Test, NativeSupportPassesThroughWithoutCpuUploads) {
  Start(false, "4");
  const auto source = CreateSource(16);
  const auto image = CreateImage(4, 4);
  const auto command = Handle<VkCommandBuffer>(10);
  Record(command, source, image, 4, 4);
  EXPECT_EQ(last_image.format, VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK);
  EXPECT_EQ(last_image.extent.width, 4U);
  EXPECT_EQ(recorded.at(command), source.buffer);
  EXPECT_FALSE(emulation->HasPendingUploads());
}

INSTANTIATE_TEST_SUITE_P(HostMemory, VulkanEtc2Test, ::testing::Bool());

}
}
