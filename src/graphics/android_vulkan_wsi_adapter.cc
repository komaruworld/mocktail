#include "mocktail/graphics/android_vulkan_wsi_adapter.h"

#include <algorithm>
#include <cstring>

namespace mocktail {
namespace graphics {
namespace {

bool Contains(const std::vector<std::string>& values,
              const std::string& candidate) {
  return std::find(values.begin(), values.end(), candidate) != values.end();
}

Status AppendUniqueExtension(const std::string& extension,
                             std::vector<std::string>* output) {
  if (extension.empty()) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "Vulkan extension names must not be empty");
  }
  if (!Contains(*output, extension)) {
    output->push_back(extension);
  }
  return Status::Ok();
}

}  // namespace

VkResult NormalizeAndroidSwapchainResult(VkResult result) {
  return result == VK_SUBOPTIMAL_KHR ? VK_ERROR_OUT_OF_DATE_KHR : result;
}

Status TranslateAndroidVulkanInstanceExtensions(
    const std::vector<std::string>& android_extensions,
    const std::vector<std::string>& host_wsi_extensions,
    std::vector<std::string>* host_extensions) {
  if (host_extensions == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "Vulkan extension output is required");
  }

  std::vector<std::string> translated;
  translated.reserve(android_extensions.size() + host_wsi_extensions.size());
  bool requested_android_surface = false;
  for (const std::string& extension : android_extensions) {
    if (extension == kAndroidSurfaceExtension) {
      requested_android_surface = true;
      continue;
    }
    Status status = AppendUniqueExtension(extension, &translated);
    if (!status.ok()) {
      return status;
    }
  }

  if (requested_android_surface) {
    if (host_wsi_extensions.empty()) {
      return Status::Error(
          StatusCode::kFailedPrecondition,
          "Android Vulkan surface requested without host WSI extensions");
    }
    for (const std::string& extension : host_wsi_extensions) {
      Status status = AppendUniqueExtension(extension, &translated);
      if (!status.ok()) {
        return status;
      }
    }
  }

  *host_extensions = std::move(translated);
  return Status::Ok();
}

Status AndroidVulkanWsiAdapter::Initialize(
    SdlVulkanWsi* host_wsi, PresentCallback present_callback,
    void* present_callback_data) {
  if (host_wsi_ != nullptr) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "Android Vulkan WSI adapter is already initialized");
  }
  if (host_wsi == nullptr || !host_wsi->IsInitialized()) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "an initialized SDL Vulkan WSI is required");
  }
  host_wsi_ = host_wsi;
  host_get_instance_proc_address_ =
      reinterpret_cast<PFN_vkGetInstanceProcAddr>(
          host_wsi->GetInstanceProcAddress());
  if (host_get_instance_proc_address_ == nullptr) {
    host_wsi_ = nullptr;
    return Status::Error(StatusCode::kUnavailable,
                         "host vkGetInstanceProcAddr is unavailable");
  }
  present_callback_ = present_callback;
  present_callback_data_ = present_callback_data;
  return Status::Ok();
}

void AndroidVulkanWsiAdapter::Shutdown() {
  host_queue_present_ = nullptr;
  host_get_device_proc_address_ = nullptr;
  host_get_instance_proc_address_ = nullptr;
  present_callback_ = nullptr;
  present_callback_data_ = nullptr;
  host_wsi_ = nullptr;
}

Status AndroidVulkanWsiAdapter::TranslateInstanceExtensions(
    const std::vector<std::string>& android_extensions,
    std::vector<std::string>* host_extensions) const {
  if (host_wsi_ == nullptr) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "Android Vulkan WSI adapter is not initialized");
  }
  return TranslateAndroidVulkanInstanceExtensions(
      android_extensions, host_wsi_->required_instance_extensions(),
      host_extensions);
}

AndroidVulkanResult AndroidVulkanWsiAdapter::CreateAndroidSurface(
    VkInstance instance, const void* android_create_info,
    const VkAllocationCallbacks* allocator, VkSurfaceKHR* surface) const {
  static_cast<void>(android_create_info);
  if (surface != nullptr) {
    *surface = VkSurfaceKHR{};
  }
  if (host_wsi_ == nullptr) {
    return kAndroidVulkanErrorInitializationFailed;
  }
  Status status = host_wsi_->CreateSurface(instance, allocator, surface);
  return status.ok() ? kAndroidVulkanSuccess
                     : kAndroidVulkanErrorInitializationFailed;
}

void AndroidVulkanWsiAdapter::DestroySurface(
    VkInstance instance, VkSurfaceKHR surface,
    const VkAllocationCallbacks* allocator) const {
  if (host_wsi_ != nullptr) {
    host_wsi_->DestroySurface(instance, surface, allocator);
  }
}

VkResult AndroidVulkanWsiAdapter::EnumerateInstanceExtensionProperties(
    const char* layer_name, std::uint32_t* property_count,
    VkExtensionProperties* properties) const {
  if (host_get_instance_proc_address_ == nullptr || property_count == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  auto enumerate = reinterpret_cast<PFN_vkEnumerateInstanceExtensionProperties>(
      host_get_instance_proc_address_(
          VkInstance{}, "vkEnumerateInstanceExtensionProperties"));
  if (enumerate == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  if (layer_name != nullptr) {
    return enumerate(layer_name, property_count, properties);
  }

  std::uint32_t host_count = 0;
  VkResult result = enumerate(nullptr, &host_count, nullptr);
  if (result != VK_SUCCESS) {
    return result;
  }
  std::vector<VkExtensionProperties> host_properties(host_count);
  if (host_count != 0) {
    result = enumerate(nullptr, &host_count, host_properties.data());
    if (result != VK_SUCCESS && result != VK_INCOMPLETE) {
      return result;
    }
    host_properties.resize(host_count);
  }

  std::vector<VkExtensionProperties> guest_properties;
  guest_properties.reserve(host_properties.size() + 1);
  const auto& host_wsi_extensions = host_wsi_->required_instance_extensions();
  for (const VkExtensionProperties& property : host_properties) {
    const std::string name(property.extensionName);
    if (!Contains(host_wsi_extensions, name) || name == "VK_KHR_surface") {
      guest_properties.push_back(property);
    }
  }
  VkExtensionProperties android_surface{};
  std::strncpy(android_surface.extensionName, kAndroidSurfaceExtension,
               sizeof(android_surface.extensionName) - 1);
  android_surface.specVersion = 6;
  guest_properties.push_back(android_surface);

  if (properties == nullptr) {
    *property_count = static_cast<std::uint32_t>(guest_properties.size());
    return VK_SUCCESS;
  }
  const std::uint32_t capacity = *property_count;
  const std::uint32_t written = std::min(
      capacity, static_cast<std::uint32_t>(guest_properties.size()));
  std::copy_n(guest_properties.begin(), written, properties);
  *property_count = written;
  return written < guest_properties.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

VkResult AndroidVulkanWsiAdapter::CreateInstance(
    const VkInstanceCreateInfo* create_info,
    const VkAllocationCallbacks* allocator, VkInstance* instance) {
  if (host_get_instance_proc_address_ == nullptr || create_info == nullptr ||
      instance == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  std::vector<std::string> requested;
  requested.reserve(create_info->enabledExtensionCount);
  for (std::uint32_t i = 0; i < create_info->enabledExtensionCount; ++i) {
    if (create_info->ppEnabledExtensionNames == nullptr ||
        create_info->ppEnabledExtensionNames[i] == nullptr) {
      return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    requested.emplace_back(create_info->ppEnabledExtensionNames[i]);
  }
  std::vector<std::string> translated;
  Status status = TranslateInstanceExtensions(requested, &translated);
  if (!status.ok()) {
    return VK_ERROR_EXTENSION_NOT_PRESENT;
  }
  std::vector<const char*> extension_names;
  extension_names.reserve(translated.size());
  for (const std::string& extension : translated) {
    extension_names.push_back(extension.c_str());
  }
  VkInstanceCreateInfo host_create_info = *create_info;
  host_create_info.enabledExtensionCount =
      static_cast<std::uint32_t>(extension_names.size());
  host_create_info.ppEnabledExtensionNames = extension_names.data();
  auto create = reinterpret_cast<PFN_vkCreateInstance>(
      host_get_instance_proc_address_(VkInstance{}, "vkCreateInstance"));
  return create != nullptr ? create(&host_create_info, allocator, instance)
                           : VK_ERROR_INITIALIZATION_FAILED;
}

PFN_vkVoidFunction AndroidVulkanWsiAdapter::GetInstanceProcAddress(
    VkInstance instance, const char* name) {
  if (host_get_instance_proc_address_ == nullptr || name == nullptr) {
    return nullptr;
  }
  if (std::strcmp(name, "vkGetDeviceProcAddr") == 0) {
    host_get_device_proc_address_ = reinterpret_cast<PFN_vkGetDeviceProcAddr>(
        host_get_instance_proc_address_(instance, name));
  }
  return host_get_instance_proc_address_(instance, name);
}

PFN_vkVoidFunction AndroidVulkanWsiAdapter::GetDeviceProcAddress(
    VkDevice device, const char* name) {
  if (host_get_device_proc_address_ == nullptr || name == nullptr) {
    return nullptr;
  }
  PFN_vkVoidFunction function = host_get_device_proc_address_(device, name);
  if (std::strcmp(name, "vkQueuePresentKHR") == 0) {
    host_queue_present_ = reinterpret_cast<PFN_vkQueuePresentKHR>(function);
  }
  return function;
}

VkResult AndroidVulkanWsiAdapter::QueuePresent(
    VkQueue queue, const VkPresentInfoKHR* present_info) {
  if (host_queue_present_ == nullptr) {
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  const VkResult result = host_queue_present_(queue, present_info);
  if ((result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR) &&
      present_callback_ != nullptr) {
    present_callback_(present_callback_data_);
  }
  return result;
}

}  // namespace graphics
}  // namespace mocktail
