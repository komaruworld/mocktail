#ifndef MOCKTAIL_AUDIO_ROBLOX_OUTPUT_DEVICE_BRIDGE_H_
#define MOCKTAIL_AUDIO_ROBLOX_OUTPUT_DEVICE_BRIDGE_H_

#include <array>
#include <cstdint>
#include <memory>
#include <mutex>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "compat/build_profile.h"
#include "mocktail/status.h"

namespace mocktail::audio {

class NativeInputCapture;
namespace internal {
struct FmodInputCaptureProfile;
}


// Replaces the profiled input/output-device query/select slots of the exact
// Build-ID-scoped FmodAudioDevice vtable. Roblox keeps owning its FMOD engine,
// while the existing settings UI sees and selects the SDL host routes that
// consume Android AudioTrack PCM.
class RobloxOutputDeviceBridge final {
 public:
  RobloxOutputDeviceBridge();
  ~RobloxOutputDeviceBridge();

  RobloxOutputDeviceBridge(const RobloxOutputDeviceBridge&) = delete;
  RobloxOutputDeviceBridge& operator=(const RobloxOutputDeviceBridge&) = delete;

  Status Install(const compat::BuildProfile& profile);
  void Shutdown();
  bool installed() const { return installed_; }
  bool active() const { return active_; }

 private:
  struct MenuDevice {
    std::uint32_t playback_device_id = 0;
    std::string name;
    std::string guid;
  };

  static bool ObserveAndroidLibrary(void* context,
                                    std::string_view logical_name,
                                    std::uintptr_t image_base);
  static int GetOutputDeviceCount(void* self);
  static void* GetOutputDeviceInfo(void* result, void* self, int index);
  static int GetCurrentOutputDevice(void* self);
  static void SetCurrentOutputDevice(void* self, int index);

  Status Activate(std::uintptr_t image_base);
  Status PatchVtableLocked();
  Status PrepareCaptureLocked();
  bool RestoreVtableLocked();
  static int GetInputDeviceCount(void *self);
  static void *GetInputDeviceInfo(void *result, void *self, int index);
  static int GetCurrentInputDevice(void *self);
  static void SetCurrentInputDevice(void *self, int index);
  int DeviceCount(bool input = false);
  void *DeviceInfo(void *result, int index, bool input = false);
  int CurrentDevice(bool input = false);
  void SelectDevice(int index, bool input = false);
  void ConstructGuestString(void* destination, std::string_view value) const;

  std::shared_ptr<NativeInputCapture> capture_;
  const internal::FmodInputCaptureProfile* capture_profile_ = nullptr;
  std::array<std::uintptr_t, 6> original_capture_methods_{};
  mutable std::mutex mutex_;
  compat::FmodOutputDeviceBridgeProfile profile_;
  std::vector<MenuDevice> devices_;
  std::vector<MenuDevice> input_devices_;
  int selected_input_index_ = 0;
  std::array<std::uintptr_t, 8> original_methods_{};
  std::uintptr_t library_base_ = 0;
  std::uintptr_t* vtable_ = nullptr;
  void* string_constructor_ = nullptr;
  int selected_index_ = 0;
  bool installed_ = false;
  bool active_ = false;
};

}  // namespace mocktail::audio

#endif  // MOCKTAIL_AUDIO_ROBLOX_OUTPUT_DEVICE_BRIDGE_H_
