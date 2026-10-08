#include "mocktail/graphics/vulkan_etc2_emulation.h"

#include "mocktail/graphics/texture_override.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mocktail::graphics {

bool LookupEmulatedEtc2Format(VkFormat format, Etc2EmulatedFormat* out) {
  Etc2EmulatedFormat mapped;
  switch (format) {
    case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
      mapped = {VK_FORMAT_R8G8B8A8_UNORM, EtcFormat::kEtc2Rgb8};
      break;
    case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
      mapped = {VK_FORMAT_R8G8B8A8_SRGB, EtcFormat::kEtc2Rgb8};
      break;
    case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
      mapped = {VK_FORMAT_R8G8B8A8_UNORM, EtcFormat::kEtc2Rgb8A1};
      break;
    case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
      mapped = {VK_FORMAT_R8G8B8A8_SRGB, EtcFormat::kEtc2Rgb8A1};
      break;
    case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
      mapped = {VK_FORMAT_R8G8B8A8_UNORM, EtcFormat::kEtc2Rgba8};
      break;
    case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
      mapped = {VK_FORMAT_R8G8B8A8_SRGB, EtcFormat::kEtc2Rgba8};
      break;
    case VK_FORMAT_EAC_R11_UNORM_BLOCK:
      mapped = {VK_FORMAT_R16_UNORM, EtcFormat::kEacR11};
      break;
    case VK_FORMAT_EAC_R11_SNORM_BLOCK:
      mapped = {VK_FORMAT_R16_SNORM, EtcFormat::kEacR11Signed};
      break;
    case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
      mapped = {VK_FORMAT_R16G16_UNORM, EtcFormat::kEacRg11};
      break;
    case VK_FORMAT_EAC_R11G11_SNORM_BLOCK:
      mapped = {VK_FORMAT_R16G16_SNORM, EtcFormat::kEacRg11Signed};
      break;
    default:
      return false;
  }
  if (out != nullptr) {
    *out = mapped;
  }
  return true;
}

VkFormatFeatureFlags EmulatedEtc2FormatFeatures(
    VkFormatFeatureFlags host_features) {
  constexpr VkFormatFeatureFlags kCompressedFeatures =
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT |
      VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
      VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
      VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
  return host_features & kCompressedFeatures;
}

bool Etc2UploadByteCounts(EtcFormat format, const VkExtent3D& extent,
                          std::uint32_t layer_count, VkDeviceSize* compressed,
                          VkDeviceSize* decoded) {
  if (extent.width == 0 || extent.height == 0 || extent.depth != 1 ||
      layer_count == 0 || compressed == nullptr || decoded == nullptr) {
    return false;
  }
  const VkDeviceSize blocks =
      ((static_cast<VkDeviceSize>(extent.width) + 3) / 4) *
      ((static_cast<VkDeviceSize>(extent.height) + 3) / 4);
  *compressed = blocks * EtcBlockBytes(format) * layer_count;
  *decoded = static_cast<VkDeviceSize>(extent.width) * extent.height *
             EtcDecodedTexelBytes(format) * layer_count;
  return true;
}

namespace {

bool EmulationDisabledByEnvironment() {
  static const bool disabled = [] {
    const char* value = std::getenv("MOCKTAIL_DISABLE_ETC2_EMULATION");
    return value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0;
  }();
  return disabled;
}

}  // namespace

bool Etc2SupportAdvertised(const char* value) {
  return value == nullptr || std::strcmp(value, "0") != 0;
}

bool Etc2SupportAdvertised() {
  static const bool advertised =
      Etc2SupportAdvertised(std::getenv("MOCKTAIL_ADVERTISE_ETC2"));
  return advertised;
}

std::uint32_t SmallTextureUpscale(const char* value) {
  if (value == nullptr || value[0] < '0' || value[0] > '9') {
    return kDefaultSmallTextureUpscale;
  }
  const long parsed = std::strtol(value, nullptr, 10);
  if (parsed < 1) {
    return 1;
  }
  return parsed > 8 ? 8 : static_cast<std::uint32_t>(parsed);
}

namespace {

bool ShouldLog(std::atomic<unsigned>* counter, unsigned limit) {
  return counter->fetch_add(1, std::memory_order_relaxed) < limit;
}

unsigned DecodeWorkerCount() {
  static const unsigned count =
      std::min(8U, std::max(1U, std::thread::hardware_concurrency() / 2));
  return count;
}

struct HostDevice {
  VkDevice device = VK_NULL_HANDLE;
  bool emulated = false;
  VkPhysicalDeviceMemoryProperties memory{};
  PFN_vkCreateImage create_image = nullptr;
  PFN_vkDestroyImage destroy_image = nullptr;
  PFN_vkCreateImageView create_image_view = nullptr;
  PFN_vkBindBufferMemory bind_buffer_memory = nullptr;
  PFN_vkBindBufferMemory2 bind_buffer_memory2 = nullptr;
  PFN_vkMapMemory map_memory = nullptr;
  PFN_vkMapMemory2 map_memory2 = nullptr;
  PFN_vkUnmapMemory unmap_memory = nullptr;
  PFN_vkUnmapMemory2 unmap_memory2 = nullptr;
  PFN_vkFreeMemory free_memory = nullptr;
  PFN_vkDestroyBuffer destroy_buffer = nullptr;
  PFN_vkCmdCopyBufferToImage copy_buffer_to_image = nullptr;
  PFN_vkCmdCopyBufferToImage2 copy_buffer_to_image2 = nullptr;
  PFN_vkCmdCopyImage copy_image = nullptr;
  PFN_vkCmdCopyImage2 copy_image2 = nullptr;
  PFN_vkCmdBlitImage blit_image = nullptr;
  PFN_vkCmdExecuteCommands execute_commands = nullptr;
  PFN_vkCreateBuffer create_buffer = nullptr;
  PFN_vkAllocateMemory allocate_memory = nullptr;
  PFN_vkGetBufferMemoryRequirements get_buffer_memory_requirements = nullptr;
};

template <typename Function>
Function DeviceProc(PFN_vkGetDeviceProcAddr get, VkDevice device,
                    const char* name, const char* alias = nullptr) {
  PFN_vkVoidFunction proc = get(device, name);
  if (proc == nullptr && alias != nullptr) {
    proc = get(device, alias);
  }
  return reinterpret_cast<Function>(proc);
}

struct BufferBinding {
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkDeviceSize offset = 0;
};

struct Mapping {
  void* data = nullptr;
  VkDeviceSize offset = 0;
  VkDeviceSize size = VK_WHOLE_SIZE;
};

struct ImageRecord {
  VkDevice device = VK_NULL_HANDLE;
  Etc2EmulatedFormat format;
  std::uint32_t scale = 1;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::shared_ptr<const RgbaImage> override;
  std::shared_ptr<const RgbaImage> base;
};

bool IsColorFormat(EtcFormat format) {
  return format == EtcFormat::kEtc2Rgb8 || format == EtcFormat::kEtc2Rgb8A1 ||
         format == EtcFormat::kEtc2Rgba8;
}

struct Staging {
  VkDevice device = VK_NULL_HANDLE;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  bool host_cached = false;
};

struct PendingUpload {
  VkDevice device = VK_NULL_HANDLE;
  VkBuffer source = VK_NULL_HANDLE;
  VkDeviceSize source_offset = 0;
  VkDeviceSize source_row_stride = 0;
  VkDeviceSize source_layer_stride = 0;
  VkDeviceSize source_span = 0;
  VkDeviceSize compressed = 0;
  VkDeviceSize decoded = 0;
  VkDeviceSize target_offset = 0;
  EtcFormat format = EtcFormat::kEtc2Rgb8;
  VkImage image = VK_NULL_HANDLE;
  std::uint32_t mip_level = 0;
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::uint32_t layers = 0;
  std::uint32_t scale = 1;
  std::uint32_t source_width = 0;
  std::uint32_t source_height = 0;
  std::uint32_t target_width = 0;
  std::uint32_t target_height = 0;
  bool full_mip = false;
  VkDeviceSize target_bytes = 0;
  std::uint8_t* target = nullptr;
  bool target_cached = false;
};

struct CommandRecord {
  std::vector<PendingUpload> uploads;
  std::vector<Staging> staging;
  std::vector<VkCommandBuffer> secondaries;
};

std::atomic<unsigned> g_image_logs{0};
std::atomic<unsigned> g_decode_logs{0};
std::atomic<unsigned> g_failure_logs{0};

void LogFailure(const char* message) {
  if (ShouldLog(&g_failure_logs, 8)) {
    std::fprintf(stderr, "  [vulkan] ETC2 emulation: %s\n", message);
  }
}

bool CreateStaging(const HostDevice& dev, VkDeviceSize size, Staging* staging,
                   std::uint8_t** mapped) {
  if (dev.create_buffer == nullptr || dev.allocate_memory == nullptr ||
      dev.get_buffer_memory_requirements == nullptr ||
      dev.bind_buffer_memory == nullptr || dev.map_memory == nullptr ||
      dev.destroy_buffer == nullptr || dev.free_memory == nullptr ||
      size == 0) {
    return false;
  }
  VkBufferCreateInfo buffer_info{};
  buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  buffer_info.size = size;
  buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkBuffer buffer = VK_NULL_HANDLE;
  if (dev.create_buffer(dev.device, &buffer_info, nullptr, &buffer) !=
      VK_SUCCESS) {
    return false;
  }
  VkMemoryRequirements requirements{};
  dev.get_buffer_memory_requirements(dev.device, buffer, &requirements);
  constexpr VkMemoryPropertyFlags kRequired =
      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
  std::uint32_t type = UINT32_MAX;
  for (std::uint32_t index = 0; index < dev.memory.memoryTypeCount; ++index) {
    if ((requirements.memoryTypeBits & (1U << index)) != 0 &&
        (dev.memory.memoryTypes[index].propertyFlags & kRequired) ==
            kRequired) {
      if (type == UINT32_MAX) {
        type = index;
      }
      if ((dev.memory.memoryTypes[index].propertyFlags &
           VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0) {
        type = index;
        break;
      }
    }
  }
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkMemoryAllocateInfo allocate_info{};
  allocate_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  allocate_info.allocationSize = requirements.size;
  allocate_info.memoryTypeIndex = type;
  void* data = nullptr;
  if (type == UINT32_MAX ||
      dev.allocate_memory(dev.device, &allocate_info, nullptr, &memory) !=
          VK_SUCCESS) {
    dev.destroy_buffer(dev.device, buffer, nullptr);
    return false;
  }
  if (dev.bind_buffer_memory(dev.device, buffer, memory, 0) != VK_SUCCESS ||
      dev.map_memory(dev.device, memory, 0, VK_WHOLE_SIZE, 0, &data) !=
          VK_SUCCESS) {
    dev.destroy_buffer(dev.device, buffer, nullptr);
    dev.free_memory(dev.device, memory, nullptr);
    return false;
  }
  *staging = {dev.device, buffer, memory,
              (dev.memory.memoryTypes[type].propertyFlags &
               VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0};
  *mapped = static_cast<std::uint8_t*>(data);
  return true;
}

void DestroyStaging(const HostDevice& dev, const Staging& staging) {
  if (dev.destroy_buffer != nullptr) {
    dev.destroy_buffer(dev.device, staging.buffer, nullptr);
  }
  if (dev.free_memory != nullptr) {
    dev.free_memory(dev.device, staging.memory, nullptr);
  }
}

// Clip block padding before scaling to the host mip.
bool ScaledAxis(std::uint32_t base, std::uint32_t scale, std::uint32_t mip,
                std::int32_t offset, std::uint32_t extent,
                std::uint32_t* texels, std::uint32_t* host_start,
                std::uint32_t* host_end) {
  const std::uint32_t app_mip = std::max<std::uint32_t>(1, base >> mip);
  const std::uint32_t host_mip =
      std::max<std::uint32_t>(1, (base * scale) >> mip);
  if (offset < 0 || static_cast<std::uint32_t>(offset) >= app_mip) {
    return false;
  }
  const std::uint32_t app_end =
      std::min(app_mip, static_cast<std::uint32_t>(offset) + extent);
  const std::uint32_t start = static_cast<std::uint32_t>(offset) * scale;
  const std::uint32_t end =
      app_end == app_mip ? host_mip : std::min(host_mip, app_end * scale);
  if (end <= start) {
    return false;
  }
  *texels = app_end - static_cast<std::uint32_t>(offset);
  *host_start = start;
  *host_end = end;
  return true;
}

bool HostRegion(const ImageRecord* record, std::uint32_t mip,
                VkOffset3D offset, VkExtent3D extent, VkOffset3D* start,
                VkOffset3D* end) {
  if (record == nullptr || record->scale <= 1) {
    *start = offset;
    *end = {offset.x + static_cast<std::int32_t>(extent.width),
            offset.y + static_cast<std::int32_t>(extent.height),
            offset.z + static_cast<std::int32_t>(extent.depth)};
    return true;
  }
  std::uint32_t texels = 0;
  std::uint32_t x0 = 0;
  std::uint32_t x1 = 0;
  std::uint32_t y0 = 0;
  std::uint32_t y1 = 0;
  if (!ScaledAxis(record->width, record->scale, mip, offset.x, extent.width,
                  &texels, &x0, &x1) ||
      !ScaledAxis(record->height, record->scale, mip, offset.y,
                  extent.height, &texels, &y0, &y1)) {
    return false;
  }
  *start = {static_cast<std::int32_t>(x0), static_cast<std::int32_t>(y0), 0};
  *end = {static_cast<std::int32_t>(x1), static_cast<std::int32_t>(y1), 1};
  return true;
}

template <typename Region>
bool PlanRegions(const ImageRecord& record, VkDevice device, VkBuffer source,
                 VkImage destination, std::uint32_t count,
                 const Region* regions, std::vector<Region>* rewritten,
                 std::vector<PendingUpload>* uploads, VkDeviceSize* total) {
  *total = 0;
  if (regions == nullptr || count == 0) {
    return false;
  }
  const std::uint32_t scale = record.scale;
  for (std::uint32_t index = 0; index < count; ++index) {
    const Region& region = regions[index];
    PendingUpload upload;
    if (!Etc2UploadByteCounts(record.format.etc_format, region.imageExtent,
                              region.imageSubresource.layerCount,
                              &upload.compressed, &upload.decoded)) {
      return false;
    }
    if (scale > 1 && region.imageSubresource.layerCount != 1) {
      return false;
    }
    upload.device = device;
    upload.source = source;
    upload.source_offset = region.bufferOffset;
    upload.target_offset = *total;
    upload.format = record.format.etc_format;
    upload.image = destination;
    upload.mip_level = region.imageSubresource.mipLevel;
    upload.width = region.imageExtent.width;
    upload.height = region.imageExtent.height;
    upload.layers = region.imageSubresource.layerCount;
    const VkDeviceSize row_texels =
        region.bufferRowLength != 0 ? region.bufferRowLength : upload.width;
    const VkDeviceSize layer_rows =
        region.bufferImageHeight != 0 ? region.bufferImageHeight : upload.height;
    if (row_texels < upload.width || layer_rows < upload.height) {
      return false;
    }
    const VkDeviceSize block_bytes = EtcBlockBytes(upload.format);
    const VkDeviceSize block_rows =
        (static_cast<VkDeviceSize>(upload.height) + 3) / 4;
    const VkDeviceSize packed_row =
        ((static_cast<VkDeviceSize>(upload.width) + 3) / 4) * block_bytes;
    upload.source_row_stride = ((row_texels + 3) / 4) * block_bytes;
    upload.source_layer_stride =
        upload.source_row_stride * ((layer_rows + 3) / 4);
    upload.source_span = (upload.layers - 1) * upload.source_layer_stride +
                         (block_rows - 1) * upload.source_row_stride + packed_row;
    upload.scale = scale;
    upload.source_width = upload.width;
    upload.source_height = upload.height;
    upload.target_width = upload.width;
    upload.target_height = upload.height;
    upload.target_bytes = upload.decoded;
    Region copy = region;
    copy.bufferOffset = *total;
    copy.bufferRowLength = 0;
    copy.bufferImageHeight = 0;
    if (scale > 1) {
      const std::uint32_t mip = region.imageSubresource.mipLevel;
      const auto axis = [&](std::uint32_t base, std::int32_t offset,
                            std::uint32_t extent, std::uint32_t* source,
                            std::uint32_t* target, std::int32_t* host_offset,
                            std::uint32_t* host_extent) {
        std::uint32_t start = 0;
        std::uint32_t end = 0;
        if (!ScaledAxis(base, scale, mip, offset, extent, source, &start,
                        &end)) {
          return false;
        }
        *target = end - start;
        *host_offset = static_cast<std::int32_t>(start);
        *host_extent = end - start;
        return true;
      };
      if (!axis(record.width, region.imageOffset.x, region.imageExtent.width,
                &upload.source_width, &upload.target_width,
                &copy.imageOffset.x, &copy.imageExtent.width) ||
          !axis(record.height, region.imageOffset.y,
                region.imageExtent.height, &upload.source_height,
                &upload.target_height, &copy.imageOffset.y,
                &copy.imageExtent.height)) {
        return false;
      }
      upload.target_bytes = static_cast<VkDeviceSize>(upload.target_width) *
                            upload.target_height *
                            EtcDecodedTexelBytes(record.format.etc_format);
      upload.full_mip =
          region.imageOffset.x == 0 && region.imageOffset.y == 0 &&
          upload.source_width == std::max<std::uint32_t>(1, record.width >> mip) &&
          upload.source_height == std::max<std::uint32_t>(1, record.height >> mip);
    }
    rewritten->push_back(copy);
    uploads->push_back(upload);
    *total += upload.target_bytes;
  }
  return true;
}

}  // namespace

struct VulkanEtc2Emulation::State {
  std::mutex mutex;
  std::shared_mutex devices_mutex;
  std::mutex commands_mutex;
  std::mutex memory_mutex;
  std::unordered_map<VkPhysicalDevice, bool> physical_devices;
  std::vector<std::unique_ptr<HostDevice>> devices;
  std::unordered_map<VkImage, ImageRecord> images;
  std::unordered_map<VkBuffer, BufferBinding> buffers;
  std::unordered_map<VkDeviceMemory, Mapping> mappings;
  std::unordered_map<VkCommandBuffer, CommandRecord> commands;
  std::atomic<bool> has_commands{false};
  TextureOverrides overrides = TextureOverrides::FromEnvironment();
  const std::uint32_t upscale =
      SmallTextureUpscale(std::getenv("MOCKTAIL_SMALL_TEXTURE_UPSCALE"));

  std::shared_ptr<const RgbaImage> FindOverride(const PendingUpload& upload,
                                                const std::uint8_t* compressed,
                                                const std::uint8_t* decoded) {
    if (!overrides.enabled() || upload.layers != 1 ||
        !IsColorFormat(upload.format)) {
      return nullptr;
    }
    std::shared_ptr<const RgbaImage> replacement;
    if (upload.mip_level == 0) {
      const std::uint64_t hash = HashBytes(compressed, upload.compressed);
      overrides.Dump(hash, upload.width, upload.height, decoded);
      replacement = overrides.Lookup(hash);
      std::lock_guard<std::mutex> lock(mutex);
      const auto record = images.find(upload.image);
      if (record != images.end()) {
        record->second.override = replacement;
      }
    } else {
      std::lock_guard<std::mutex> lock(mutex);
      const auto record = images.find(upload.image);
      if (record != images.end()) {
        replacement = record->second.override;
      }
    }
    return replacement;
  }

  void EmitUpload(const PendingUpload& upload, const std::uint8_t* compressed,
                  const std::uint8_t* decoded) {
    const std::shared_ptr<const RgbaImage> replacement =
        FindOverride(upload, compressed, decoded);
    if (replacement != nullptr) {
      ResampleRgba(*replacement, upload.target_width, upload.target_height,
                   upload.target);
    } else if (upload.scale > 1) {
      std::shared_ptr<const RgbaImage> base;
      if (upload.full_mip) {
        std::lock_guard<std::mutex> lock(mutex);
        const auto record = images.find(upload.image);
        if (record != images.end()) {
          if (upload.mip_level == 0) {
            auto level0 = std::make_shared<RgbaImage>();
            level0->width = upload.source_width;
            level0->height = upload.source_height;
            level0->pixels.assign(decoded, decoded + upload.decoded);
            if (upload.source_width != upload.width ||
                upload.source_height != upload.height) {
              level0->pixels.clear();
              for (std::uint32_t y = 0; y < upload.source_height; ++y) {
                const std::uint8_t* row =
                    decoded + static_cast<std::size_t>(y) * upload.width * 4;
                level0->pixels.insert(level0->pixels.end(), row,
                                      row + upload.source_width * 4);
              }
            }
            record->second.base = level0;
          }
          base = record->second.base;
        }
      }
      if (base != nullptr) {
        ResampleRgba(*base, upload.target_width, upload.target_height,
                     upload.target);
        return;
      }
      const std::uint8_t* source = decoded;
      std::vector<std::uint8_t> cropped;
      if (upload.source_width != upload.width ||
          upload.source_height != upload.height) {
        const std::size_t row = static_cast<std::size_t>(upload.source_width) * 4;
        cropped.resize(row * upload.source_height);
        for (std::uint32_t y = 0; y < upload.source_height; ++y) {
          std::memcpy(cropped.data() + y * row,
                      decoded + static_cast<std::size_t>(y) * upload.width * 4,
                      row);
        }
        source = cropped.data();
      }
      ResampleRgba(source, upload.source_width, upload.source_height,
                   upload.target_width, upload.target_height, upload.target);
    } else {
      std::memcpy(upload.target, decoded, upload.decoded);
    }
  }

  const HostDevice* Find(VkDevice device) {
    std::shared_lock<std::shared_mutex> lock(devices_mutex);
    for (const auto& candidate : devices) {
      if (candidate->device == device) {
        return candidate.get();
      }
    }
    return nullptr;
  }

  bool BlitScaledCopy(const HostDevice& dev, VkCommandBuffer command_buffer,
                      VkImage source, VkImageLayout source_layout,
                      VkImage destination, VkImageLayout destination_layout,
                      std::uint32_t count, const VkImageCopy* regions);

  bool LookupImage(const HostDevice& dev, VkImage image,
                   ImageRecord* record) {
    if (!dev.emulated) {
      return false;
    }
    std::lock_guard<std::mutex> lock(mutex);
    const auto found = images.find(image);
    if (found == images.end()) {
      return false;
    }
    *record = found->second;
    return true;
  }

  void Record(VkCommandBuffer command_buffer,
              std::vector<PendingUpload> uploads, const Staging& staging) {
    std::lock_guard<std::mutex> lock(commands_mutex);
    CommandRecord& record = commands[command_buffer];
    record.uploads.insert(record.uploads.end(), uploads.begin(),
                          uploads.end());
    record.staging.push_back(staging);
    has_commands.store(true, std::memory_order_release);
  }
};

VulkanEtc2Emulation::VulkanEtc2Emulation() : state_(new State) {}

VulkanEtc2Emulation::~VulkanEtc2Emulation() { delete state_; }

bool VulkanEtc2Emulation::PhysicalDeviceNeedsEmulation(
    VkPhysicalDevice physical_device, PFN_vkGetPhysicalDeviceFeatures host) {
  if (EmulationDisabledByEnvironment() || host == nullptr ||
      physical_device == VK_NULL_HANDLE) {
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto found = state_->physical_devices.find(physical_device);
    if (found != state_->physical_devices.end()) {
      return found->second;
    }
  }
  VkPhysicalDeviceFeatures features{};
  host(physical_device, &features);
  const bool needed = features.textureCompressionETC2 != VK_TRUE;
  std::lock_guard<std::mutex> lock(state_->mutex);
  state_->physical_devices[physical_device] = needed;
  return needed;
}

void VulkanEtc2Emulation::RegisterDevice(
    VkDevice device, VkPhysicalDevice physical_device, bool emulated,
    const VkPhysicalDeviceMemoryProperties& memory,
    PFN_vkGetDeviceProcAddr get_device_proc_addr) {
  static_cast<void>(physical_device);
  if (device == VK_NULL_HANDLE || get_device_proc_addr == nullptr) {
    return;
  }
  auto dev = std::make_unique<HostDevice>();
  const auto get = get_device_proc_addr;
  dev->device = device;
  dev->emulated = emulated;
  dev->memory = memory;
  dev->create_image = DeviceProc<PFN_vkCreateImage>(get, device, "vkCreateImage");
  dev->destroy_image =
      DeviceProc<PFN_vkDestroyImage>(get, device, "vkDestroyImage");
  dev->create_image_view =
      DeviceProc<PFN_vkCreateImageView>(get, device, "vkCreateImageView");
  dev->bind_buffer_memory =
      DeviceProc<PFN_vkBindBufferMemory>(get, device, "vkBindBufferMemory");
  dev->bind_buffer_memory2 = DeviceProc<PFN_vkBindBufferMemory2>(
      get, device, "vkBindBufferMemory2", "vkBindBufferMemory2KHR");
  dev->map_memory = DeviceProc<PFN_vkMapMemory>(get, device, "vkMapMemory");
  dev->map_memory2 = DeviceProc<PFN_vkMapMemory2>(get, device, "vkMapMemory2",
                                                  "vkMapMemory2KHR");
  dev->unmap_memory =
      DeviceProc<PFN_vkUnmapMemory>(get, device, "vkUnmapMemory");
  dev->unmap_memory2 = DeviceProc<PFN_vkUnmapMemory2>(
      get, device, "vkUnmapMemory2", "vkUnmapMemory2KHR");
  dev->free_memory = DeviceProc<PFN_vkFreeMemory>(get, device, "vkFreeMemory");
  dev->destroy_buffer =
      DeviceProc<PFN_vkDestroyBuffer>(get, device, "vkDestroyBuffer");
  dev->copy_buffer_to_image = DeviceProc<PFN_vkCmdCopyBufferToImage>(
      get, device, "vkCmdCopyBufferToImage");
  dev->copy_buffer_to_image2 = DeviceProc<PFN_vkCmdCopyBufferToImage2>(
      get, device, "vkCmdCopyBufferToImage2", "vkCmdCopyBufferToImage2KHR");
  dev->copy_image = DeviceProc<PFN_vkCmdCopyImage>(get, device, "vkCmdCopyImage");
  dev->copy_image2 = DeviceProc<PFN_vkCmdCopyImage2>(
      get, device, "vkCmdCopyImage2", "vkCmdCopyImage2KHR");
  dev->blit_image = DeviceProc<PFN_vkCmdBlitImage>(get, device, "vkCmdBlitImage");
  dev->execute_commands =
      DeviceProc<PFN_vkCmdExecuteCommands>(get, device, "vkCmdExecuteCommands");
  dev->create_buffer =
      DeviceProc<PFN_vkCreateBuffer>(get, device, "vkCreateBuffer");
  dev->allocate_memory =
      DeviceProc<PFN_vkAllocateMemory>(get, device, "vkAllocateMemory");
  dev->get_buffer_memory_requirements =
      DeviceProc<PFN_vkGetBufferMemoryRequirements>(
          get, device, "vkGetBufferMemoryRequirements");
  if (emulated) {
    std::fprintf(stderr,
                 "  [vulkan] ETC2/EAC emulation enabled: compressed uploads "
                 "decode to RGBA8/R16 host images\n");
  }
  std::lock_guard<std::shared_mutex> lock(state_->devices_mutex);
  state_->devices.erase(
      std::remove_if(state_->devices.begin(), state_->devices.end(),
                     [device](const std::unique_ptr<HostDevice>& candidate) {
                       return candidate->device == device;
                     }),
      state_->devices.end());
  state_->devices.push_back(std::move(dev));
}

void VulkanEtc2Emulation::DestroyDevice(VkDevice device) {
  std::vector<Staging> doomed;
  {
    std::lock_guard<std::mutex> lock(state_->commands_mutex);
    for (auto it = state_->commands.begin(); it != state_->commands.end();) {
      const bool owned = std::any_of(
          it->second.staging.begin(), it->second.staging.end(),
          [device](const Staging& staging) { return staging.device == device; });
      if (owned) {
        doomed.insert(doomed.end(), it->second.staging.begin(),
                      it->second.staging.end());
        it = state_->commands.erase(it);
      } else {
        ++it;
      }
    }
    state_->has_commands.store(!state_->commands.empty(),
                               std::memory_order_release);
  }
  {
    std::lock_guard<std::mutex> lock(state_->mutex);
    for (auto it = state_->images.begin(); it != state_->images.end();) {
      it = it->second.device == device ? state_->images.erase(it) : std::next(it);
    }
  }
  if (const HostDevice* dev = state_->Find(device); dev != nullptr) {
    for (const Staging& staging : doomed) {
      DestroyStaging(*dev, staging);
    }
  }
  std::lock_guard<std::shared_mutex> lock(state_->devices_mutex);
  state_->devices.erase(
      std::remove_if(state_->devices.begin(), state_->devices.end(),
                     [device](const std::unique_ptr<HostDevice>& candidate) {
                       return candidate->device == device;
                     }),
      state_->devices.end());
}

VkResult VulkanEtc2Emulation::CreateImage(VkDevice device,
                                          const VkImageCreateInfo* create_info,
                                          const VkAllocationCallbacks* allocator,
                                          VkImage* image) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->create_image == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  Etc2EmulatedFormat format;
  if (!dev->emulated || create_info == nullptr ||
      !LookupEmulatedEtc2Format(create_info->format, &format)) {
    return dev->create_image(device, create_info, allocator, image);
  }
  VkImageCreateInfo host_info = *create_info;
  host_info.format = format.host_format;
  host_info.flags &= ~static_cast<VkImageCreateFlags>(
      VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT);
  std::uint32_t scale = 1;
  if (state_->upscale > 1 && IsColorFormat(format.etc_format) &&
      create_info->imageType == VK_IMAGE_TYPE_2D &&
      create_info->arrayLayers == 1 && create_info->extent.depth == 1 &&
      create_info->extent.width <= kSmallTextureMaxExtent &&
      create_info->extent.height <= kSmallTextureMaxExtent) {
    scale = state_->upscale;
    host_info.extent.width *= scale;
    host_info.extent.height *= scale;
  }
  const VkResult result = dev->create_image(device, &host_info, allocator, image);
  if (result == VK_SUCCESS && image != nullptr) {
    {
      std::lock_guard<std::mutex> lock(state_->mutex);
      state_->images[*image] = {device,
                                format,
                                scale,
                                create_info->extent.width,
                                create_info->extent.height,
                                nullptr,
                                nullptr};
    }
    if (ShouldLog(&g_image_logs, 4)) {
      std::fprintf(stderr,
                   "  [vulkan] ETC2 image %ux%u mips=%u format=%d -> host "
                   "format=%d\n",
                   create_info->extent.width, create_info->extent.height,
                   create_info->mipLevels,
                   static_cast<int>(create_info->format),
                   static_cast<int>(format.host_format));
    }
  }
  return result;
}

void VulkanEtc2Emulation::DestroyImage(VkDevice device, VkImage image,
                                       const VkAllocationCallbacks* allocator) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->destroy_image == nullptr) {
    return;
  }
  if (dev->emulated) {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->images.erase(image);
  }
  dev->destroy_image(device, image, allocator);
}

VkResult VulkanEtc2Emulation::CreateImageView(
    VkDevice device, const VkImageViewCreateInfo* create_info,
    const VkAllocationCallbacks* allocator, VkImageView* view) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->create_image_view == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  Etc2EmulatedFormat format;
  if (!dev->emulated || create_info == nullptr ||
      !LookupEmulatedEtc2Format(create_info->format, &format)) {
    return dev->create_image_view(device, create_info, allocator, view);
  }
  VkImageViewCreateInfo host_info = *create_info;
  host_info.format = format.host_format;
  return dev->create_image_view(device, &host_info, allocator, view);
}

VkResult VulkanEtc2Emulation::BindBufferMemory(VkDevice device, VkBuffer buffer,
                                               VkDeviceMemory memory,
                                               VkDeviceSize offset) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->bind_buffer_memory == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result =
      dev->bind_buffer_memory(device, buffer, memory, offset);
  if (result == VK_SUCCESS && dev->emulated) {
    std::lock_guard<std::mutex> lock(state_->memory_mutex);
    state_->buffers[buffer] = {memory, offset};
  }
  return result;
}

VkResult VulkanEtc2Emulation::BindBufferMemory2(
    VkDevice device, std::uint32_t count, const VkBindBufferMemoryInfo* infos) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->bind_buffer_memory2 == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result = dev->bind_buffer_memory2(device, count, infos);
  if (result == VK_SUCCESS && dev->emulated && infos != nullptr) {
    std::lock_guard<std::mutex> lock(state_->memory_mutex);
    for (std::uint32_t index = 0; index < count; ++index) {
      state_->buffers[infos[index].buffer] = {infos[index].memory,
                                              infos[index].memoryOffset};
    }
  }
  return result;
}

VkResult VulkanEtc2Emulation::MapMemory(VkDevice device, VkDeviceMemory memory,
                                        VkDeviceSize offset, VkDeviceSize size,
                                        VkMemoryMapFlags flags, void** data) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->map_memory == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  std::unique_lock<std::mutex> lock(state_->memory_mutex, std::defer_lock);
  if (dev->emulated) {
    lock.lock();
  }
  const VkResult result =
      dev->map_memory(device, memory, offset, size, flags, data);
  if (result == VK_SUCCESS && dev->emulated && data != nullptr) {
    state_->mappings[memory] = {*data, offset, size};
  }
  return result;
}

VkResult VulkanEtc2Emulation::MapMemory2(VkDevice device,
                                         const VkMemoryMapInfo* info,
                                         void** data) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->map_memory2 == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  std::unique_lock<std::mutex> lock(state_->memory_mutex, std::defer_lock);
  if (dev->emulated) {
    lock.lock();
  }
  const VkResult result = dev->map_memory2(device, info, data);
  if (result == VK_SUCCESS && dev->emulated && info != nullptr &&
      data != nullptr) {
    state_->mappings[info->memory] = {*data, info->offset, info->size};
  }
  return result;
}

void VulkanEtc2Emulation::UnmapMemory(VkDevice device, VkDeviceMemory memory) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->unmap_memory == nullptr) {
    return;
  }
  std::unique_lock<std::mutex> lock(state_->memory_mutex, std::defer_lock);
  if (dev->emulated) {
    lock.lock();
    state_->mappings.erase(memory);
  }
  dev->unmap_memory(device, memory);
}

VkResult VulkanEtc2Emulation::UnmapMemory2(VkDevice device,
                                           const VkMemoryUnmapInfo* info) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->unmap_memory2 == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  std::unique_lock<std::mutex> lock(state_->memory_mutex, std::defer_lock);
  if (dev->emulated && info != nullptr) {
    lock.lock();
    state_->mappings.erase(info->memory);
  }
  return dev->unmap_memory2(device, info);
}

void VulkanEtc2Emulation::FreeMemory(VkDevice device, VkDeviceMemory memory,
                                     const VkAllocationCallbacks* allocator) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->free_memory == nullptr) {
    return;
  }
  std::unique_lock<std::mutex> lock(state_->memory_mutex, std::defer_lock);
  if (dev->emulated) {
    lock.lock();
    state_->mappings.erase(memory);
  }
  dev->free_memory(device, memory, allocator);
}

void VulkanEtc2Emulation::DestroyBuffer(VkDevice device, VkBuffer buffer,
                                        const VkAllocationCallbacks* allocator) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->destroy_buffer == nullptr) {
    return;
  }
  if (dev->emulated) {
    std::lock_guard<std::mutex> lock(state_->memory_mutex);
    state_->buffers.erase(buffer);
  }
  dev->destroy_buffer(device, buffer, allocator);
}

void VulkanEtc2Emulation::CmdCopyBufferToImage(
    VkDevice device, VkCommandBuffer command_buffer, VkBuffer source,
    VkImage destination, VkImageLayout layout, std::uint32_t region_count,
    const VkBufferImageCopy* regions) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->copy_buffer_to_image == nullptr) {
    return;
  }
  ImageRecord record;
  if (!state_->LookupImage(*dev, destination, &record)) {
    dev->copy_buffer_to_image(command_buffer, source, destination, layout,
                              region_count, regions);
    return;
  }
  std::vector<VkBufferImageCopy> rewritten;
  std::vector<PendingUpload> uploads;
  VkDeviceSize total = 0;
  Staging staging;
  std::uint8_t* mapped = nullptr;
  if (!PlanRegions(record, device, source, destination, region_count, regions,
                   &rewritten, &uploads, &total)) {
    LogFailure("skipped an upload with an unsupported region");
    return;
  }
  if (!CreateStaging(*dev, total, &staging, &mapped)) {
    LogFailure("could not allocate a host-visible staging buffer");
    return;
  }
  for (PendingUpload& upload : uploads) {
    upload.target = mapped + upload.target_offset;
    upload.target_cached = staging.host_cached;
  }
  state_->Record(command_buffer, std::move(uploads), staging);
  dev->copy_buffer_to_image(command_buffer, staging.buffer, destination,
                            layout, region_count, rewritten.data());
}

void VulkanEtc2Emulation::CmdCopyBufferToImage2(
    VkDevice device, VkCommandBuffer command_buffer,
    const VkCopyBufferToImageInfo2* info) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->copy_buffer_to_image2 == nullptr ||
      info == nullptr) {
    return;
  }
  ImageRecord record;
  if (!state_->LookupImage(*dev, info->dstImage, &record)) {
    dev->copy_buffer_to_image2(command_buffer, info);
    return;
  }
  std::vector<VkBufferImageCopy2> rewritten;
  std::vector<PendingUpload> uploads;
  VkDeviceSize total = 0;
  Staging staging;
  std::uint8_t* mapped = nullptr;
  if (!PlanRegions(record, device, info->srcBuffer, info->dstImage,
                   info->regionCount, info->pRegions, &rewritten, &uploads,
                   &total)) {
    LogFailure("skipped an upload with an unsupported region");
    return;
  }
  if (!CreateStaging(*dev, total, &staging, &mapped)) {
    LogFailure("could not allocate a host-visible staging buffer");
    return;
  }
  for (PendingUpload& upload : uploads) {
    upload.target = mapped + upload.target_offset;
    upload.target_cached = staging.host_cached;
  }
  state_->Record(command_buffer, std::move(uploads), staging);
  VkCopyBufferToImageInfo2 host_info = *info;
  host_info.srcBuffer = staging.buffer;
  host_info.pRegions = rewritten.data();
  dev->copy_buffer_to_image2(command_buffer, &host_info);
}

bool VulkanEtc2Emulation::State::BlitScaledCopy(
    const HostDevice& dev, VkCommandBuffer command_buffer, VkImage source,
    VkImageLayout source_layout, VkImage destination,
    VkImageLayout destination_layout, std::uint32_t count,
    const VkImageCopy* regions) {
  ImageRecord source_record;
  ImageRecord destination_record;
  const bool source_scaled =
      LookupImage(dev, source, &source_record) && source_record.scale > 1;
  const bool destination_scaled =
      LookupImage(dev, destination, &destination_record) &&
      destination_record.scale > 1;
  if ((!source_scaled && !destination_scaled) || dev.blit_image == nullptr ||
      regions == nullptr) {
    return false;
  }
  std::vector<VkImageBlit> blits;
  blits.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    const VkImageCopy& region = regions[index];
    VkImageBlit blit{};
    blit.srcSubresource = region.srcSubresource;
    blit.dstSubresource = region.dstSubresource;
    if (!HostRegion(source_scaled ? &source_record : nullptr,
                    region.srcSubresource.mipLevel, region.srcOffset,
                    region.extent, &blit.srcOffsets[0], &blit.srcOffsets[1]) ||
        !HostRegion(destination_scaled ? &destination_record : nullptr,
                    region.dstSubresource.mipLevel, region.dstOffset,
                    region.extent, &blit.dstOffsets[0], &blit.dstOffsets[1])) {
      LogFailure("skipped an image copy with an unsupported region");
      return true;
    }
    blits.push_back(blit);
  }
  const bool same_scale = source_scaled && destination_scaled &&
                          source_record.scale == destination_record.scale;
  dev.blit_image(command_buffer, source, source_layout, destination,
                 destination_layout, static_cast<std::uint32_t>(blits.size()),
                 blits.data(), same_scale ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
  return true;
}

void VulkanEtc2Emulation::CmdCopyImage(
    VkDevice device, VkCommandBuffer command_buffer, VkImage source,
    VkImageLayout source_layout, VkImage destination,
    VkImageLayout destination_layout, std::uint32_t region_count,
    const VkImageCopy* regions) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->copy_image == nullptr) {
    return;
  }
  if (dev->emulated && state_->upscale > 1 &&
      state_->BlitScaledCopy(*dev, command_buffer, source, source_layout,
                             destination, destination_layout, region_count,
                             regions)) {
    return;
  }
  dev->copy_image(command_buffer, source, source_layout, destination,
                  destination_layout, region_count, regions);
}

void VulkanEtc2Emulation::CmdCopyImage2(VkDevice device,
                                        VkCommandBuffer command_buffer,
                                        const VkCopyImageInfo2* info) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->copy_image2 == nullptr || info == nullptr) {
    return;
  }
  if (dev->emulated && state_->upscale > 1 && info->pRegions != nullptr) {
    std::vector<VkImageCopy> regions;
    regions.reserve(info->regionCount);
    for (std::uint32_t index = 0; index < info->regionCount; ++index) {
      const VkImageCopy2& region = info->pRegions[index];
      regions.push_back({region.srcSubresource, region.srcOffset,
                         region.dstSubresource, region.dstOffset,
                         region.extent});
    }
    if (state_->BlitScaledCopy(*dev, command_buffer, info->srcImage,
                               info->srcImageLayout, info->dstImage,
                               info->dstImageLayout, info->regionCount,
                               regions.data())) {
      return;
    }
  }
  dev->copy_image2(command_buffer, info);
}

void VulkanEtc2Emulation::CmdExecuteCommands(
    VkDevice device, VkCommandBuffer command_buffer, std::uint32_t count,
    const VkCommandBuffer* secondaries) {
  const HostDevice* dev = state_->Find(device);
  if (dev == nullptr || dev->execute_commands == nullptr) {
    return;
  }
  if (dev->emulated && secondaries != nullptr &&
      state_->has_commands.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> lock(state_->commands_mutex);
    for (std::uint32_t index = 0; index < count; ++index) {
      if (state_->commands.count(secondaries[index]) != 0) {
        state_->commands[command_buffer].secondaries.push_back(
            secondaries[index]);
      }
    }
  }
  dev->execute_commands(command_buffer, count, secondaries);
}

bool VulkanEtc2Emulation::HasPendingUploads() const {
  return state_->has_commands.load(std::memory_order_acquire);
}

void VulkanEtc2Emulation::PrepareSubmit(const VkCommandBuffer* command_buffers,
                                        std::uint32_t count) {
  if (command_buffers == nullptr || count == 0 ||
      !state_->has_commands.load(std::memory_order_acquire)) {
    return;
  }
  struct Work {
    PendingUpload upload;
    std::uint8_t* source = nullptr;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkDeviceSize memory_offset = 0;
  };
  std::vector<PendingUpload> uploads;
  {
    std::lock_guard<std::mutex> lock(state_->commands_mutex);
    std::unordered_set<VkCommandBuffer> visited;
    auto collect = [this, &uploads, &visited](VkCommandBuffer command_buffer) {
      const auto record = state_->commands.find(command_buffer);
      if (record == state_->commands.end() ||
          !visited.insert(command_buffer).second) {
        return;
      }
      uploads.insert(uploads.end(), record->second.uploads.begin(),
                      record->second.uploads.end());
    };
    for (std::uint32_t index = 0; index < count; ++index) {
      collect(command_buffers[index]);
      const auto record = state_->commands.find(command_buffers[index]);
      if (record != state_->commands.end()) {
        const std::vector<VkCommandBuffer> secondaries =
            record->second.secondaries;
        for (VkCommandBuffer secondary : secondaries) {
          collect(secondary);
        }
      }
    }
  }
  if (uploads.empty()) {
    return;
  }
  std::vector<Work> work;
  work.reserve(uploads.size());
  std::unique_lock<std::mutex> memory_lock(state_->memory_mutex);
  {
    for (const PendingUpload& upload : uploads) {
      const auto binding = state_->buffers.find(upload.source);
      if (binding == state_->buffers.end()) {
        LogFailure("upload source buffer has no tracked memory binding");
        continue;
      }
      Work item;
      item.upload = upload;
      const VkDeviceSize absolute =
          binding->second.offset + upload.source_offset;
      const auto mapping = state_->mappings.find(binding->second.memory);
      if (mapping == state_->mappings.end()) {
        item.memory = binding->second.memory;
        item.memory_offset = absolute;
      } else if (absolute >= mapping->second.offset &&
                 (mapping->second.size == VK_WHOLE_SIZE ||
                  (absolute - mapping->second.offset <= mapping->second.size &&
                   upload.source_span <= mapping->second.size -
                                            (absolute - mapping->second.offset)))) {
        item.source = static_cast<std::uint8_t*>(mapping->second.data) +
                      (absolute - mapping->second.offset);
      } else {
        LogFailure("upload source lies outside the mapped range");
        continue;
      }
      work.push_back(item);
    }
  }
  if (work.empty()) {
    return;
  }
  // Decode in cached RAM; mapped Vulkan memory makes scattered reads slow.
  // Map each source allocation once: Vulkan forbids concurrent mappings.
  struct TemporaryMapping {
    const HostDevice* dev = nullptr;
    std::uint8_t* data = nullptr;
  };
  std::unordered_map<VkDeviceMemory, TemporaryMapping> temporary;
  struct Copy {
    const std::uint8_t* source = nullptr;
    VkDeviceSize compressed = 0;
    VkDeviceSize decoded = 0;
  };
  std::vector<Copy> copies;
  std::vector<const PendingUpload*> copied_uploads;
  VkDeviceSize compressed_total = 0;
  VkDeviceSize decoded_total = 0;
  for (const Work& item : work) {
    const PendingUpload& upload = item.upload;
    const std::uint8_t* source = item.source;
    if (source == nullptr) {
      auto found = temporary.find(item.memory);
      if (found == temporary.end()) {
        const HostDevice* dev = state_->Find(upload.device);
        void* data = nullptr;
        if (dev == nullptr || dev->map_memory == nullptr ||
            dev->map_memory(dev->device, item.memory, 0, VK_WHOLE_SIZE, 0,
                            &data) != VK_SUCCESS) {
          LogFailure("could not map an unmapped upload source");
          continue;
        }
        found = temporary
                    .emplace(item.memory,
                             TemporaryMapping{
                                 dev, static_cast<std::uint8_t*>(data)})
                    .first;
      }
      source = found->second.data + item.memory_offset;
    }
    const bool direct = upload.target_cached && upload.scale == 1 &&
                        !state_->overrides.enabled();
    const VkDeviceSize decoded = direct ? 0 : upload.decoded;
    copies.push_back({source, upload.compressed, decoded});
    copied_uploads.push_back(&upload);
    compressed_total += upload.compressed;
    decoded_total += decoded;
  }

  static thread_local std::vector<std::uint8_t> scratch_source;
  static thread_local std::vector<std::uint8_t> scratch_decoded;
  if (scratch_source.size() < compressed_total) {
    scratch_source.resize(compressed_total);
  }
  if (scratch_decoded.size() < decoded_total) {
    scratch_decoded.resize(decoded_total);
  }
  std::vector<EtcDecodeJob> jobs;
  VkDeviceSize compressed_offset = 0;
  VkDeviceSize decoded_offset = 0;
  for (std::size_t index = 0; index < copies.size(); ++index) {
    const Copy& copy = copies[index];
    const PendingUpload& upload = *copied_uploads[index];
    std::uint8_t* source = scratch_source.data() + compressed_offset;
    std::uint8_t* decoded = copy.decoded == 0
                                ? upload.target
                                : scratch_decoded.data() + decoded_offset;
    const VkDeviceSize compressed_layer = upload.compressed / upload.layers;
    const VkDeviceSize decoded_layer = upload.decoded / upload.layers;
    if (upload.source_span == copy.compressed) {
      std::memcpy(source, copy.source, copy.compressed);
    } else {
      const VkDeviceSize block_rows =
          (static_cast<VkDeviceSize>(upload.height) + 3) / 4;
      const VkDeviceSize packed_row = compressed_layer / block_rows;
      for (std::uint32_t layer = 0; layer < upload.layers; ++layer) {
        for (VkDeviceSize row = 0; row < block_rows; ++row) {
          std::memcpy(source + layer * compressed_layer + row * packed_row,
                      copy.source + layer * upload.source_layer_stride +
                          row * upload.source_row_stride,
                      packed_row);
        }
      }
    }
    for (std::uint32_t layer = 0; layer < upload.layers; ++layer) {
      EtcDecodeJob job;
      job.format = upload.format;
      job.source = source + layer * compressed_layer;
      job.source_bytes = compressed_layer;
      job.width = upload.width;
      job.height = upload.height;
      job.destination = decoded + layer * decoded_layer;
      job.destination_bytes = decoded_layer;
      jobs.push_back(job);
    }
    compressed_offset += copy.compressed;
    decoded_offset += copy.decoded;
    if (ShouldLog(&g_decode_logs, 4)) {
      std::fprintf(stderr,
                   "  [vulkan] ETC2 upload decoded %ux%u layers=%u\n",
                   upload.width, upload.height, upload.layers);
    }
  }
  for (const auto& [memory, mapping] : temporary) {
    if (mapping.dev->unmap_memory != nullptr) {
      mapping.dev->unmap_memory(mapping.dev->device, memory);
    }
  }
  memory_lock.unlock();
  DecodeEtcJobs(jobs.data(), jobs.size(), DecodeWorkerCount());
  for (const EtcDecodeJob& job : jobs) {
    if (!job.ok) {
      LogFailure("could not decode an upload layer");
    }
  }
  compressed_offset = 0;
  decoded_offset = 0;
  for (std::size_t index = 0; index < copied_uploads.size(); ++index) {
    if (copies[index].decoded != 0) {
      state_->EmitUpload(*copied_uploads[index],
                         scratch_source.data() + compressed_offset,
                         scratch_decoded.data() + decoded_offset);
    }
    compressed_offset += copies[index].compressed;
    decoded_offset += copies[index].decoded;
  }
}

void VulkanEtc2Emulation::ReleaseCommandBuffer(VkCommandBuffer command_buffer) {
  if (!state_->has_commands.load(std::memory_order_acquire)) {
    return;
  }
  std::vector<Staging> doomed;
  {
    std::lock_guard<std::mutex> lock(state_->commands_mutex);
    const auto record = state_->commands.find(command_buffer);
    if (record == state_->commands.end()) {
      return;
    }
    doomed = std::move(record->second.staging);
    state_->commands.erase(record);
    state_->has_commands.store(!state_->commands.empty(),
                               std::memory_order_release);
  }
  for (const Staging& staging : doomed) {
    if (const HostDevice* dev = state_->Find(staging.device); dev != nullptr) {
      DestroyStaging(*dev, staging);
    }
  }
}

}  // namespace mocktail::graphics
