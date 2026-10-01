#include "mocktail/audio/roblox_output_device_bridge.h"

#include <elf.h>
#include <sys/mman.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cinttypes>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "audio/native_input_capture.h"
#include "audio/roblox_output_device_bridge_internal.h"
#include "linker/linker.h"
#include "mocktail/audio/sdl_audio_sink.h"

namespace mocktail::audio {
namespace {

constexpr std::size_t kOutputCountVtableIndex = 5;
constexpr std::size_t kOutputInfoVtableIndex = 6;
constexpr std::size_t kCurrentOutputVtableIndex = 7;
constexpr std::size_t kSelectOutputVtableIndex = 17;
constexpr std::size_t kGuestStringSize = 24;
constexpr std::size_t kGuestDeviceInfoSize = kGuestStringSize * 2 + 1;
constexpr std::size_t kStringConstructorContractSize = 24;
constexpr std::size_t kMaximumMenuDevices = 128;
constexpr std::size_t kMaximumDeviceNameBytes = 512;
constexpr std::size_t kFirstPatchedVtableIndex = 4;

// Microphone capture on Build 2998 (Roblox 2.736.1408 x86_64). Roblox 2.738
// inserted device-list methods at slots 7 and 11; main verified its capture
// methods at {4, 13..17}, between the input current (12) and select (18)
// slots, so 2998 keeps them at {4, 11..15}, between input current (10) and
// select (16). The input slots, with main's input RVAs for this image, anchor
// that layout before anything is patched.
constexpr std::string_view kCaptureBuildId =
    "ade08266c67aee88ec9c1d00902150e1684dad3a";
constexpr std::uintptr_t kCaptureVtableRva = 0x6c58040;
// Format, latency, start, stop, poll, recording.
constexpr std::array<std::size_t, 6> kCaptureSlots{4, 11, 12, 13, 14, 15};
constexpr std::array<std::size_t, 4> kInputAnchorSlots{8, 9, 10, 16};
constexpr std::array<std::uintptr_t, 4> kInputAnchorRvas{
    0x32d12dc, 0x32d12ea, 0x32d13a6, 0x32d0a56};
// On 2.738 the capture methods lie within 0x1400 bytes of the input methods.
constexpr std::uintptr_t kCaptureCodeWindow = 0x4000;

std::atomic<RobloxOutputDeviceBridge*> g_active_bridge{nullptr};

Status InvalidArgument(std::string message) {
  return Status::Error(StatusCode::kInvalidArgument, std::move(message));
}

Status FailedPrecondition(std::string message) {
  return Status::Error(StatusCode::kFailedPrecondition, std::move(message));
}

bool IsValidElfImage(std::uintptr_t image_base) {
  if (image_base == 0) {
    return false;
  }
  const auto* header = reinterpret_cast<const Elf64_Ehdr*>(image_base);
  return std::memcmp(header->e_ident, ELFMAG, SELFMAG) == 0 &&
         header->e_ident[EI_CLASS] == ELFCLASS64 && header->e_phoff != 0 &&
         header->e_phnum != 0;
}

bool IsProgramHeaderRange(std::uintptr_t image_base, std::uintptr_t rva,
                          std::size_t size, std::uint32_t type,
                          std::uint32_t required_flags) {
  if (!IsValidElfImage(image_base) || size == 0 ||
      rva > std::numeric_limits<std::uintptr_t>::max() - size) {
    return false;
  }
  const auto* header = reinterpret_cast<const Elf64_Ehdr*>(image_base);
  const auto* program_headers = reinterpret_cast<const Elf64_Phdr*>(
      image_base + static_cast<std::uintptr_t>(header->e_phoff));
  const std::uintptr_t range_end = rva + size;
  for (std::size_t index = 0; index < header->e_phnum; ++index) {
    const Elf64_Phdr& segment = program_headers[index];
    if (segment.p_type != type ||
        (segment.p_flags & required_flags) != required_flags ||
        segment.p_vaddr >
            std::numeric_limits<std::uintptr_t>::max() - segment.p_memsz) {
      continue;
    }
    const std::uintptr_t segment_end =
        static_cast<std::uintptr_t>(segment.p_vaddr + segment.p_memsz);
    if (rva >= segment.p_vaddr && range_end <= segment_end) {
      return true;
    }
  }
  return false;
}

bool IsExecutableImageRange(std::uintptr_t image_base, std::uintptr_t rva,
                            std::size_t size) {
  return IsProgramHeaderRange(image_base, rva, size, PT_LOAD, PF_R | PF_X);
}

bool IsRelroImageRange(std::uintptr_t image_base, std::uintptr_t rva,
                       std::size_t size) {
  return IsProgramHeaderRange(image_base, rva, size, PT_GNU_RELRO, 0);
}

bool SetVtableWritable(std::uintptr_t* vtable, bool writable) {
  if (vtable == nullptr) {
    return false;
  }
  const long page_size_value = sysconf(_SC_PAGESIZE);
  if (page_size_value <= 0) {
    return false;
  }
  const std::uintptr_t page_size = static_cast<std::uintptr_t>(page_size_value);
  const std::uintptr_t first =
      reinterpret_cast<std::uintptr_t>(vtable + kFirstPatchedVtableIndex);
  const std::uintptr_t last =
      reinterpret_cast<std::uintptr_t>(vtable + kSelectOutputVtableIndex + 1);
  if (last <= first) {
    return false;
  }
  const std::uintptr_t page_begin = first - first % page_size;
  if (last > std::numeric_limits<std::uintptr_t>::max() - page_size + 1) {
    return false;
  }
  const std::uintptr_t page_end =
      ((last + page_size - 1) / page_size) * page_size;
  const int protection = writable ? PROT_READ | PROT_WRITE : PROT_READ;
  return mprotect(reinterpret_cast<void*>(page_begin), page_end - page_begin,
                  protection) == 0;
}

template <typename Function>
std::uintptr_t FunctionAddress(Function function) {
  return reinterpret_cast<std::uintptr_t>(function);
}

}  // namespace

namespace internal {

bool HasExpectedFmodStringConstructorContract(const std::uint8_t* code,
                                              std::size_t size) {
  constexpr std::array<std::uint8_t, kStringConstructorContractSize>
      kExpectedPrefix = {
          0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
          0x41, 0x54, 0x53, 0x48, 0x89, 0xd3, 0x49, 0x89,
          0xf6, 0x49, 0x89, 0xff, 0x48, 0x83, 0xfa, 0x16,
      };
  return code != nullptr && size >= kExpectedPrefix.size() &&
         std::memcmp(code, kExpectedPrefix.data(), kExpectedPrefix.size()) == 0;
}

bool HasExpectedFmodOutputDeviceVtable(
    const std::uintptr_t* vtable, std::uintptr_t image_base,
    const compat::FmodOutputDeviceBridgeProfile& profile) {
  return vtable != nullptr && image_base != 0 &&
         vtable[kOutputCountVtableIndex] ==
             image_base + profile.count_method_rva &&
         vtable[kOutputInfoVtableIndex] ==
             image_base + profile.info_method_rva &&
         vtable[kCurrentOutputVtableIndex] ==
             image_base + profile.current_method_rva &&
         vtable[kSelectOutputVtableIndex] ==
             image_base + profile.select_method_rva;
}

std::string MakeOutputDeviceGuid(std::uint32_t playback_device_id,
                                 std::string_view name) {
  if (playback_device_id == 0) {
    return "mocktail:default";
  }
  std::uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char byte : name) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  for (std::size_t shift = 0; shift < sizeof(playback_device_id) * CHAR_BIT;
       shift += CHAR_BIT) {
    hash ^= (playback_device_id >> shift) & 0xffU;
    hash *= 1099511628211ULL;
  }
  std::array<char, 20> encoded{};
  std::snprintf(encoded.data(), encoded.size(), "mt:%016" PRIx64, hash);
  return encoded.data();
}

}  // namespace internal

RobloxOutputDeviceBridge::RobloxOutputDeviceBridge() = default;

RobloxOutputDeviceBridge::~RobloxOutputDeviceBridge() { Shutdown(); }

Status RobloxOutputDeviceBridge::Install(const compat::BuildProfile& profile) {
  if (installed_) {
    return FailedPrecondition("Roblox output-device bridge is already armed");
  }
  if (!profile.fmod_output_device_bridge.has_value()) {
    return Status::Ok();
  }
  if (!profile.allow_host_abi_bridges) {
    return FailedPrecondition(
        "FMOD output-device profile requires host ABI bridges");
  }

  RobloxOutputDeviceBridge* expected = nullptr;
  if (!g_active_bridge.compare_exchange_strong(expected, this,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire)) {
    return FailedPrecondition(
        "another Roblox output-device bridge owns the process");
  }
  profile_ = *profile.fmod_output_device_bridge;
  const char* native_capture = std::getenv("MOCKTAIL_NATIVE_INPUT_CAPTURE");
  capture_profile_supported_ =
      profile.elf_build_id == kCaptureBuildId &&
      profile_.vtable_rva == kCaptureVtableRva &&
      !(native_capture != nullptr && std::strcmp(native_capture, "0") == 0);
  if (!linker::SetAndroidLibraryLoadObserver(&ObserveAndroidLibrary, this)) {
    g_active_bridge.store(nullptr, std::memory_order_release);
    profile_ = {};
    return FailedPrecondition(
        "Android library load observer already has an owner");
  }
  installed_ = true;
  std::fprintf(stderr,
               "  [audio-menu] host output bridge armed for Build ID %s\n",
               profile.elf_build_id.c_str());
  return Status::Ok();
}

void RobloxOutputDeviceBridge::Shutdown() {
  if (!installed_) {
    return;
  }
  linker::ClearAndroidLibraryLoadObserver(this);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_ && !RestoreVtableLocked()) {
      std::fprintf(stderr,
                   "  [audio-menu] failed to restore FmodAudioDevice "
                   "vtable protection\n");
    }
    NativeInputCapture::Retire(std::move(capture_));
    active_ = false;
    devices_.clear();
    string_constructor_ = nullptr;
    library_base_ = 0;
    vtable_ = nullptr;
    selected_index_ = 0;
  }
  RobloxOutputDeviceBridge* expected = this;
  (void)g_active_bridge.compare_exchange_strong(
      expected, nullptr, std::memory_order_acq_rel, std::memory_order_acquire);
  profile_ = {};
  installed_ = false;
}

bool RobloxOutputDeviceBridge::ObserveAndroidLibrary(
    void* context, std::string_view logical_name, std::uintptr_t image_base) {
  if (logical_name != "libroblox") {
    return true;
  }
  auto* bridge = static_cast<RobloxOutputDeviceBridge*>(context);
  if (bridge == nullptr) {
    return false;
  }
  const Status status = bridge->Activate(image_base);
  if (!status.ok()) {
    std::fprintf(stderr, "  [audio-menu] activation failed: %s\n",
                 status.message().c_str());
    return false;
  }
  return true;
}

Status RobloxOutputDeviceBridge::Activate(std::uintptr_t image_base) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!installed_) {
    return FailedPrecondition("Roblox output-device bridge is not armed");
  }
  if (active_) {
    return image_base == library_base_
               ? Status::Ok()
               : FailedPrecondition(
                     "Roblox output-device bridge saw a second image");
  }
  if (!IsValidElfImage(image_base)) {
    return InvalidArgument("libroblox image base is not a valid ELF64 image");
  }
  const std::uintptr_t largest_rva =
      std::max({profile_.vtable_rva, profile_.string_constructor_rva,
                profile_.count_method_rva, profile_.info_method_rva,
                profile_.current_method_rva, profile_.select_method_rva});
  if (image_base > std::numeric_limits<std::uintptr_t>::max() - largest_rva) {
    return InvalidArgument("FMOD output-device profile address overflows");
  }

  std::vector<SdlPlaybackDevice> playback_devices;
  Status status = ListSdlPlaybackDevices(&playback_devices);
  if (!status.ok()) {
    return status;
  }
  if (playback_devices.size() + 1 > kMaximumMenuDevices) {
    return InvalidArgument("host exposes too many audio output devices");
  }

  std::uint32_t configured_device_id = 0;
  std::string configured_name;
  status =
      GetConfiguredSdlPlaybackDevice(&configured_device_id, &configured_name);
  if (!status.ok()) {
    return status;
  }

  devices_.clear();
  devices_.reserve(playback_devices.size() + 1);
  devices_.push_back(
      {0, "System Default", internal::MakeOutputDeviceGuid(0, "default")});
  selected_index_ = 0;
  for (const SdlPlaybackDevice& device : playback_devices) {
    if (device.name.empty() || device.name.size() > kMaximumDeviceNameBytes) {
      devices_.clear();
      return InvalidArgument("SDL audio output name is empty or too long");
    }
    devices_.push_back(
        {device.id, device.name,
         internal::MakeOutputDeviceGuid(device.id, device.name)});
    if (device.id == configured_device_id) {
      selected_index_ = static_cast<int>(devices_.size() - 1);
    }
  }
  if (configured_device_id != 0 && selected_index_ == 0) {
    devices_.clear();
    return FailedPrecondition("configured SDL audio output disappeared");
  }

  library_base_ = image_base;
  vtable_ = reinterpret_cast<std::uintptr_t*>(image_base + profile_.vtable_rva);
  string_constructor_ =
      reinterpret_cast<void*>(image_base + profile_.string_constructor_rva);
  status = PatchVtableLocked();
  if (!status.ok()) {
    devices_.clear();
    string_constructor_ = nullptr;
    library_base_ = 0;
    vtable_ = nullptr;
    return status;
  }
  active_ = true;

  std::fprintf(stderr,
               "  [audio-menu] exposed %zu host output routes; selected=%s\n",
               devices_.size(), devices_[selected_index_].name.c_str());
  for (const MenuDevice& device : devices_) {
    std::fprintf(stderr, "    [audio-menu] %s\n", device.name.c_str());
  }
  return Status::Ok();
}

Status RobloxOutputDeviceBridge::PatchVtableLocked() {
  if (profile_.vtable_rva >
      std::numeric_limits<std::uintptr_t>::max() -
          (kSelectOutputVtableIndex + 1) * sizeof(std::uintptr_t)) {
    return FailedPrecondition("FMOD output-device vtable range overflows");
  }
  const std::uintptr_t first_slot_rva =
      profile_.vtable_rva + kFirstPatchedVtableIndex * sizeof(std::uintptr_t);
  const std::size_t slot_range_size =
      (kSelectOutputVtableIndex - kFirstPatchedVtableIndex + 1) *
      sizeof(std::uintptr_t);
  if (!IsRelroImageRange(library_base_, first_slot_rva, slot_range_size) ||
      !IsExecutableImageRange(library_base_, profile_.string_constructor_rva,
                              kStringConstructorContractSize) ||
      !IsExecutableImageRange(library_base_, profile_.count_method_rva, 1) ||
      !IsExecutableImageRange(library_base_, profile_.info_method_rva, 1) ||
      !IsExecutableImageRange(library_base_, profile_.current_method_rva, 1) ||
      !IsExecutableImageRange(library_base_, profile_.select_method_rva, 1)) {
    return FailedPrecondition(
        "FMOD output-device profile points outside verified image ranges");
  }
  if (!internal::HasExpectedFmodOutputDeviceVtable(vtable_, library_base_,
                                                   profile_)) {
    return FailedPrecondition(
        "FmodAudioDevice output vtable does not match the Build-ID profile");
  }
  const auto* constructor_code = reinterpret_cast<const std::uint8_t*>(
      library_base_ + profile_.string_constructor_rva);
  if (!internal::HasExpectedFmodStringConstructorContract(
          constructor_code, kStringConstructorContractSize)) {
    return FailedPrecondition(
        "FmodAudioDevice string ABI contract validation failed");
  }

  if (capture_profile_supported_) {
    // The output routes do not depend on capture; keep them if it is refused.
    const Status capture_status = PrepareCaptureLocked();
    if (!capture_status.ok()) {
      std::fprintf(stderr, "  [audio-input] native capture unavailable: %s\n",
                   capture_status.message().c_str());
    }
  }

  original_methods_ = {
      vtable_[kOutputCountVtableIndex],
      vtable_[kOutputInfoVtableIndex],
      vtable_[kCurrentOutputVtableIndex],
      vtable_[kSelectOutputVtableIndex],
  };
  if (!SetVtableWritable(vtable_, true)) {
    capture_.reset();
    return Status::Error(StatusCode::kPlatformError,
                         "cannot make FmodAudioDevice vtable writable");
  }

  __atomic_store_n(&vtable_[kOutputCountVtableIndex],
                   FunctionAddress(&GetOutputDeviceCount), __ATOMIC_RELEASE);
  __atomic_store_n(&vtable_[kOutputInfoVtableIndex],
                   FunctionAddress(&GetOutputDeviceInfo), __ATOMIC_RELEASE);
  __atomic_store_n(&vtable_[kCurrentOutputVtableIndex],
                   FunctionAddress(&GetCurrentOutputDevice), __ATOMIC_RELEASE);
  __atomic_store_n(&vtable_[kSelectOutputVtableIndex],
                   FunctionAddress(&SetCurrentOutputDevice), __ATOMIC_RELEASE);
  if (capture_) {
    g_native_capture.store(capture_.get(), std::memory_order_release);
    const std::array<std::uintptr_t, 6> methods{
        FunctionAddress(&NativeInputCapture::Format),
        FunctionAddress(&NativeInputCapture::Latency),
        FunctionAddress(&NativeInputCapture::Start),
        FunctionAddress(&NativeInputCapture::Stop),
        FunctionAddress(&NativeInputCapture::Poll),
        FunctionAddress(&NativeInputCapture::Recording)};
    for (std::size_t i = 0; i < kCaptureSlots.size(); ++i)
      __atomic_store_n(&vtable_[kCaptureSlots[i]], methods[i], __ATOMIC_RELEASE);
  }
  if (!SetVtableWritable(vtable_, false)) {
    __atomic_store_n(&vtable_[kOutputCountVtableIndex], original_methods_[0],
                     __ATOMIC_RELEASE);
    __atomic_store_n(&vtable_[kOutputInfoVtableIndex], original_methods_[1],
                     __ATOMIC_RELEASE);
    __atomic_store_n(&vtable_[kCurrentOutputVtableIndex], original_methods_[2],
                     __ATOMIC_RELEASE);
    __atomic_store_n(&vtable_[kSelectOutputVtableIndex], original_methods_[3],
                     __ATOMIC_RELEASE);
    if (capture_) {
      for (std::size_t i = 0; i < kCaptureSlots.size(); ++i)
        __atomic_store_n(&vtable_[kCaptureSlots[i]],
                         original_capture_methods_[i], __ATOMIC_RELEASE);
      NativeInputCapture::Retire(std::move(capture_));
    }
    (void)SetVtableWritable(vtable_, false);
    return Status::Error(StatusCode::kPlatformError,
                         "cannot restore FmodAudioDevice RELRO protection");
  }
  if (capture_) {
    std::fprintf(stderr,
                 "  [audio-input] native capture armed for Build ID %s "
                 "(slots 4/11-15 = %#" PRIxPTR "/%#" PRIxPTR "/%#" PRIxPTR
                 "/%#" PRIxPTR "/%#" PRIxPTR "/%#" PRIxPTR ")\n",
                 kCaptureBuildId.data(),
                 original_capture_methods_[0] - library_base_,
                 original_capture_methods_[1] - library_base_,
                 original_capture_methods_[2] - library_base_,
                 original_capture_methods_[3] - library_base_,
                 original_capture_methods_[4] - library_base_,
                 original_capture_methods_[5] - library_base_);
  }
  return Status::Ok();
}

Status RobloxOutputDeviceBridge::PrepareCaptureLocked() {
  for (std::size_t i = 0; i < kInputAnchorSlots.size(); ++i) {
    if (vtable_[kInputAnchorSlots[i]] != library_base_ + kInputAnchorRvas[i]) {
      return FailedPrecondition(
          "FmodAudioDevice input slots do not match Build 2998");
    }
  }
  const auto [lowest_input, highest_input] =
      std::minmax_element(kInputAnchorRvas.begin(), kInputAnchorRvas.end());
  for (std::size_t i = 0; i < kCaptureSlots.size(); ++i) {
    const std::uintptr_t method = vtable_[kCaptureSlots[i]];
    const std::uintptr_t rva = method - library_base_;
    if (method < library_base_ ||
        !IsExecutableImageRange(library_base_, rva, 1) ||
        rva + kCaptureCodeWindow < *lowest_input ||
        rva > *highest_input + kCaptureCodeWindow) {
      return FailedPrecondition(
          "FmodAudioDevice capture slot is outside the device code");
    }
    const bool repeated =
        std::find(original_capture_methods_.begin(),
                  original_capture_methods_.begin() + i,
                  method) != original_capture_methods_.begin() + i ||
        std::find(kInputAnchorRvas.begin(), kInputAnchorRvas.end(), rva) !=
            kInputAnchorRvas.end();
    if (repeated) {
      return FailedPrecondition(
          "FmodAudioDevice capture slots are not distinct methods");
    }
    original_capture_methods_[i] = method;
  }
  const char* input_device = std::getenv("MOCKTAIL_AUDIO_INPUT_DEVICE");
  const bool enabled =
      input_device == nullptr || std::strcmp(input_device, "disabled") != 0;
  capture_ = std::make_unique<NativeInputCapture>(
      NativeInputCapture::SinkAbi{
          library_base_, &IsRelroImageRange, &IsExecutableImageRange,
          reinterpret_cast<NativeInputCapture::OriginalLatency>(
              original_capture_methods_[1])},
      enabled);
  return Status::Ok();
}

bool RobloxOutputDeviceBridge::RestoreVtableLocked() {
  if (vtable_ == nullptr || !SetVtableWritable(vtable_, true)) {
    return false;
  }
  __atomic_store_n(&vtable_[kOutputCountVtableIndex], original_methods_[0],
                   __ATOMIC_RELEASE);
  __atomic_store_n(&vtable_[kOutputInfoVtableIndex], original_methods_[1],
                   __ATOMIC_RELEASE);
  __atomic_store_n(&vtable_[kCurrentOutputVtableIndex], original_methods_[2],
                   __ATOMIC_RELEASE);
  __atomic_store_n(&vtable_[kSelectOutputVtableIndex], original_methods_[3],
                   __ATOMIC_RELEASE);
  if (capture_) {
    for (std::size_t i = 0; i < kCaptureSlots.size(); ++i)
      __atomic_store_n(&vtable_[kCaptureSlots[i]], original_capture_methods_[i],
                       __ATOMIC_RELEASE);
  }
  return SetVtableWritable(vtable_, false);
}

int RobloxOutputDeviceBridge::GetOutputDeviceCount(void* self) {
  static_cast<void>(self);
  RobloxOutputDeviceBridge* bridge =
      g_active_bridge.load(std::memory_order_acquire);
  return bridge != nullptr ? bridge->DeviceCount() : 0;
}

void* RobloxOutputDeviceBridge::GetOutputDeviceInfo(void* result, void* self,
                                                    int index) {
  static_cast<void>(self);
  RobloxOutputDeviceBridge* bridge =
      g_active_bridge.load(std::memory_order_acquire);
  if (bridge == nullptr) {
    if (result != nullptr) {
      std::memset(result, 0, kGuestDeviceInfoSize);
    }
    return result;
  }
  return bridge->DeviceInfo(result, index);
}

int RobloxOutputDeviceBridge::GetCurrentOutputDevice(void* self) {
  static_cast<void>(self);
  RobloxOutputDeviceBridge* bridge =
      g_active_bridge.load(std::memory_order_acquire);
  return bridge != nullptr ? bridge->CurrentDevice() : 0;
}

void RobloxOutputDeviceBridge::SetCurrentOutputDevice(void* self, int index) {
  static_cast<void>(self);
  RobloxOutputDeviceBridge* bridge =
      g_active_bridge.load(std::memory_order_acquire);
  if (bridge != nullptr) {
    bridge->SelectDevice(index);
  }
}

int RobloxOutputDeviceBridge::DeviceCount() {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_ ? static_cast<int>(devices_.size()) : 0;
}

void* RobloxOutputDeviceBridge::DeviceInfo(void* result, int index) {
  if (result == nullptr) {
    return nullptr;
  }
  std::memset(result, 0, kGuestDeviceInfoSize);
  std::lock_guard<std::mutex> lock(mutex_);
  if (!active_ || index < 0 ||
      static_cast<std::size_t>(index) >= devices_.size()) {
    return result;
  }
  const MenuDevice& device = devices_[static_cast<std::size_t>(index)];
  ConstructGuestString(result, device.name);
  ConstructGuestString(static_cast<std::uint8_t*>(result) + kGuestStringSize,
                       device.guid);
  static_cast<std::uint8_t*>(result)[kGuestStringSize * 2] = 1;
  return result;
}

int RobloxOutputDeviceBridge::CurrentDevice() {
  std::lock_guard<std::mutex> lock(mutex_);
  return active_ ? selected_index_ : 0;
}

void RobloxOutputDeviceBridge::SelectDevice(int index) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!active_ || index < 0 ||
      static_cast<std::size_t>(index) >= devices_.size()) {
    return;
  }
  const MenuDevice& device = devices_[static_cast<std::size_t>(index)];
  std::string resolved_name;
  const Status status =
      SwitchSdlPlaybackDevice(device.playback_device_id, &resolved_name);
  if (!status.ok()) {
    std::fprintf(stderr, "  [audio-menu] cannot select host output '%s': %s\n",
                 device.name.c_str(), status.message().c_str());
    return;
  }
  selected_index_ = index;
  std::fprintf(stderr, "  [audio-menu] selected host output=%s\n",
               resolved_name.c_str());
}

void RobloxOutputDeviceBridge::ConstructGuestString(
    void* destination, std::string_view value) const {
  using GuestStringConstructor = void* (*)(void*, const char*, std::size_t);
  auto constructor =
      reinterpret_cast<GuestStringConstructor>(string_constructor_);
  constructor(destination, value.data(), value.size());
}

}  // namespace mocktail::audio
