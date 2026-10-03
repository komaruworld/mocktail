#ifndef MOCKTAIL_VR_ROBLOX_VR_DEVICE_BRIDGE_H_
#define MOCKTAIL_VR_ROBLOX_VR_DEVICE_BRIDGE_H_

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string_view>

#include "compat/build_profile.h"
#include "mocktail/status.h"

namespace mocktail::vr {
namespace internal {
struct VrBridgeTestAccess;
struct VrBuildContract;
}

const compat::VrDebugDeviceBridgeProfile* FindRobloxVrDeviceProfile(
    std::string_view build_id);

// Experimental native stereo bring-up for the exact Build-ID-scoped
// RBX::Graphics::DebugDeviceVR object of the guest client.
//
// The bridge interposes the state and eye getters of the exact guest build.
// Resource readiness is checked on the live object during each state call;
// initialization never dereferences cached guest object/device pointers. The
// guest owns resource destruction. Recreated objects and reused addresses
// therefore need no cache invalidation. Initialization happens only on a thread
// observed presenting.
class RobloxVrDeviceBridge final {
public:
  using StateGetterFn = void *(*)(void *result_buffer, void *device_object);
  using EyeGetterFn = void *(*)(void *device_object, int eye_index);
  using EyeInitializerFn = void (*)(void *device_object, void *graphics_device);

  RobloxVrDeviceBridge() = default;
  ~RobloxVrDeviceBridge();

  RobloxVrDeviceBridge(const RobloxVrDeviceBridge &) = delete;
  RobloxVrDeviceBridge &operator=(const RobloxVrDeviceBridge &) = delete;

  // Arms the bridge for an exact-build profile that carries
  // vr_debug_device_bridge metadata. Claims the process-wide bridge slot.
  Status Install(const compat::BuildProfile &profile);

  // Validates machine contracts at the loaded image base and interposes the
  // two DebugDeviceVR vtable slots. Called once, after libroblox relocations
  // and before any guest startup code runs.
  Status Activate(std::uintptr_t image_base);

  // Restores the interposed vtable slots and disarms. Safe to call twice.
  void Shutdown();

  bool installed() const { return installed_; }
  bool active() const { return active_.load(std::memory_order_acquire); }

  // Render-thread notification from the Vulkan adapter's host present path.
  void NoteHostPresent();

  // Guest-side hooks invoked by the interposed vtable stubs.
  void OnHapticsCall(void* device_object, int hand, float amplitude);
  void OnPointerFrameCall(void* result, void* service);
  void OnStateGetterCall(void *result_buffer, void *device_object);
  void OnEyeGetterCall(void *device_object, int eye_index, void *framebuffer);

  StateGetterFn original_state_getter() const {
    return original_state_getter_.load(std::memory_order_acquire);
  }
  EyeGetterFn original_eye_getter() const {
    return original_eye_getter_.load(std::memory_order_acquire);
  }

  // Cumulative initialization diagnostics (not a count of live devices).
  std::uintptr_t image_base() const {
    return image_base_.load(std::memory_order_acquire);
  }
  int captured_device_count() const;
  int initialized_device_count() const;
  std::uint64_t present_count() const {
    return present_count_.load(std::memory_order_relaxed);
  }
  std::uint64_t eye_getter_call_count(int eye_index) const;

private:
  friend struct internal::VrBridgeTestAccess;
  static constexpr std::uint64_t kEyeGetterSummaryInterval = 600;
  bool ValidateObjectHeader(void *self) const;
  void *ValidatedGraphicsDevice(void *self) const;
  void TryInitializeEyes(void *self);
  bool RestoreVtableLocked();

  mutable std::mutex mutex_;
  compat::VrDebugDeviceBridgeProfile profile_{};
  const internal::VrBuildContract* contract_ = nullptr;
  bool installed_ = false;
  std::atomic<bool> active_{false};
  std::atomic<std::uintptr_t> image_base_{0};
  std::atomic<std::uintptr_t> image_size_{0};
  std::uintptr_t *vtable_ = nullptr;
  std::uintptr_t original_haptic_slot_ = 0;
  std::uintptr_t original_state_slot_ = 0;
  std::uintptr_t original_eye_slot_ = 0;
  std::atomic<StateGetterFn> original_state_getter_{nullptr};
  std::atomic<EyeGetterFn> original_eye_getter_{nullptr};
  std::atomic<std::uint64_t> present_thread_{0};
  std::atomic<std::uint64_t> present_count_{0};
  std::atomic<bool> flag_warning_logged_{false};
  std::atomic<int> captured_count_{0};
  std::atomic<int> initialized_count_{0};
  mutable std::atomic<std::uint64_t> eye_call_totals_[2] = {};
};

// Process-wide single active bridge, mirroring the audio output-device
// bridge ownership model. Install claims the slot; the destructor releases
// it. legacy_runtime forwards the loaded libroblox image base through
// NotifyRobloxVrImageLoaded; it is a successful no-op while no bridge is
// armed, and fail-closed once one is.
Status NotifyRobloxVrImageLoaded(std::uintptr_t image_base);

} // namespace mocktail::vr

#endif // MOCKTAIL_VR_ROBLOX_VR_DEVICE_BRIDGE_H_
