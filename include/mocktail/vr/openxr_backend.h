#ifndef MOCKTAIL_VR_OPENXR_BACKEND_H_
#define MOCKTAIL_VR_OPENXR_BACKEND_H_

#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "mocktail/status.h"
#include "mocktail/vr/vr_mirror.h"
#include "mocktail/vr/xr_actions.h"
#include "mocktail/vr/xr_controller.h"

namespace mocktail::vr {

// Resolved --vr output mode. kXrOutput drives the real Roblox stereo images
// into an OpenXR projection layer; kNativeOnly keeps the diagnostic native
// stereo prototype without any runtime requirement. Selected through
// MOCKTAIL_VR_BACKEND ("xr" default, "native" diagnostic).
enum class VrBackendMode {
  kDisabled,
  kNativeOnly,
  kXrOutput,
};

// Explicit XR_RUNTIME_JSON wins. Runtime preference can select SteamVR (ALVR),
// WiVRn, or the registered OpenXR runtime without changing system registration.
std::string SelectVrRuntimeManifest(const std::string& explicit_manifest,
                                   const std::vector<std::string>& candidates);
std::vector<std::string> VrRuntimeManifestCandidates(
    const std::string &preference, const std::string &home,
    const std::string &config_home, const std::string &config_dirs);
struct VrRuntimeDiscovery {
  std::vector<std::string> candidates;
  std::string reason;
};
VrRuntimeDiscovery DiscoverVrRuntime(
    const std::string& preference, const std::string& home,
    const std::string& config_home, const std::string& config_dirs,
    const std::string& runtime_dir, const std::string& proc_root = "/proc");
std::string VrRuntimeUnavailableHint(bool user_selected, const std::string& manifest);
VrBackendMode ResolveVrBackendMode(bool vr_enabled);
const char* VrBackendModeName(VrBackendMode mode);

// One deterministic head pose sample. Position is metres, orientation is a
// quaternion (x, y, z, w) in the OpenXR LOCAL reference space.
// VrHandPose (tracked controller hands) is defined in xr_controller.h.
struct ScriptedPoseSample {
  float position[3] = {0.f, 0.f, 0.f};
  float orientation[4] = {0.f, 0.f, 0.f, 1.f};
  float eye_offset[2][3] = {};  // metres in head space
  float eye_fov[2][4] = {};  // left, right, up, down angles
  // Index 0 = left, 1 = right (Roblox UserCFrame LeftHand/RightHand).
  VrHandPose hands[2];
  // Exact 2998 DebugDeviceVR input channels, published with the same pose.
  float controller_channels[28] = {};
  bool controllers_connected = false;
  std::uint64_t frame = 0;
  bool valid = false;
};

// Production OpenXR backend for the native stereo prototype.
//
// Lifecycle: main arms the backend before Roblox startup (xrCreateInstance
// and xrGetSystem fail fast with a clear message when no runtime exists).
// The Vulkan adapter delegates the guest's vkCreateInstance/vkCreateDevice
// through xrCreateVulkanInstanceKHR/xrCreateVulkanDeviceKHR so the runtime's
// extension and GPU requirements are satisfied by the very device Roblox
// renders with. The session is created on that device; per-frame XR work runs
// on the render thread from the adapter's host-present notification:
//   xrEndFrame(N) with the two eye images of frame N copied into the
//   projection swapchains, then xrWaitFrame/xrBeginFrame/xrLocateViews for
//   frame N+1 and publishing the scripted/runtime head pose that the device
//   bridge injects into Roblox before its eye passes run.
//
// EGL/OpenGL ES uses XR_MNDX_egl_enable on the guest SDL context and GPU
// framebuffer blits into GLES swapchain textures. It shares the same frame
// scheduling, tracking, pose application and session recovery below.
//
// Eye image provenance is recorded in the adapter during the guest eye
// initializer (owner = DebugDeviceVR object) and bound to an eye index only
// through the real per-frame eye-getter request plus the actual render-pass
// attachment, mirroring the validated evidence-layer semantics. Creation
// order or size alone never selects an eye.
//
// All public entry points are exception-free at their boundary; the
// implementation is compiled with exceptions like openxr_preview.cc and
// translates failures into Status/log outcomes.
class GlesTransport;
enum class VrGraphicsApi { kVulkan, kOpenGles };

class OpenXrBackend final {
 public:
  OpenXrBackend() = default;
  ~OpenXrBackend();
  OpenXrBackend(const OpenXrBackend&) = delete;
  OpenXrBackend& operator=(const OpenXrBackend&) = delete;

  // Claims the process-wide backend slot and creates XrInstance/system.
  Status Arm(VrGraphicsApi graphics_api = VrGraphicsApi::kVulkan);
  // EGL context must be current on the calling thread. Resolver bypasses guest
  // hooks.
  Status AttachGlesContext(void *display, void *config, void *context,
                           void *egl_get_proc, void *(*resolve)(const char *));
  void DetachGlesContext(void *context);
  void NoteGlesPresent();
  void *WrapGlesProcAddress(const char *name, void *raw);
  // Idempotent full teardown (XR resources first, then the instance).
  void Disarm();
  bool armed() const { return armed_.load(std::memory_order_acquire); }

  // Adapter delegation. Return false when the backend does not own creation
  // (caller falls back to the plain host loader path).
  bool CreateVulkanInstance(const VkInstanceCreateInfo* create_info,
                            const VkAllocationCallbacks* allocator,
                            VkInstance* instance, VkResult* result);
  bool CreateVulkanDevice(VkPhysicalDevice physical_device,
                          const VkDeviceCreateInfo* create_info,
                          const VkAllocationCallbacks* allocator,
                          VkDevice* device, VkResult* result);
  void NoteDeviceDestroyed(VkDevice device);
  void NoteInstanceDestroyed(VkInstance instance);
  void NoteQueue(VkDevice device, VkQueue queue, std::uint32_t family,
                 std::uint32_t index);

  // Resource provenance recording (adapter wrappers).
  bool WantsResourceRecords() const {
    return recording_.load(std::memory_order_acquire);
  }
  void RecordImage(VkDevice device, VkImage image,
                   const VkImageCreateInfo* info);
  void RecordImageView(VkImageView view, VkImage image);
  void RecordFramebuffer(VkFramebuffer framebuffer,
                         const VkFramebufferCreateInfo* info);
  void RecordRenderPass(VkRenderPass render_pass,
                        const VkRenderPassCreateInfo* info);
  void NoteDestroyImage(VkImage image);
  void NoteDestroyImageView(VkImageView view);
  void NoteDestroyFramebuffer(VkFramebuffer framebuffer);
  void NoteRenderPassBegin(VkCommandBuffer command_buffer,
                           const VkRenderPassBeginInfo* info);
  bool EyeBindingComplete() const {
    return eyes_bound_.load(std::memory_order_acquire);
  }

  // Render-thread frame cycle at host present begin.
  void NoteHostPresent(VkQueue queue, VkDevice device);
  void RecordDesktopSwapchain(VkDevice device, VkSwapchainKHR swapchain,
      const VkSwapchainCreateInfoKHR* info, const VkImage* images, unsigned count);
  // On success the original present semaphores have been consumed and the
  // copy has completed. The caller must present with no wait semaphores.
  VkResult MirrorDesktop(VkQueue queue, const VkPresentInfoKHR* info);

  // Pose published for the frame that is about to render. The device bridge
  // reads it once per frame on the render thread and injects it into the
  // guest. valid=false until the XR session located a usable head pose.
  // hands[0]=left, hands[1]=right carry this frame's tracked controller grip
  // poses (metres, LOCAL space) when a controller layer is active.
  ScriptedPoseSample PublishedHeadPose() const;
  bool RecommendedEyeExtent(std::uint32_t* width, std::uint32_t* height) const;
  void NotePoseApplied(void* owner, std::uint64_t frame);

  // Controller input delivery, consumed by the window-thread input runtime
  // through the C ABI in xr_controller_abi.h. TakeControllerDelivery pops one
  // batch produced by the once-per-frame action sync; it returns false when
  // no controller layer is active or nothing is queued. The snapshot exposes
  // the current levels for overflow resync. Haptics requests are thread-safe.
  bool TakeControllerDelivery(ControllerDelivery* out);
  ControllerSnapshot ControllerSnapshotForFrame() const;
  void RequestControllerHaptics(int hand, float amplitude,
                                std::uint64_t duration_ns, float frequency_hz);

  void StopControllerHaptics(int hand);

  // Diagnostics.
  std::uint64_t submitted_frames() const {
    return submitted_frames_.load(std::memory_order_relaxed);
  }
  std::uint64_t skipped_frames() const {
    return skipped_frames_.load(std::memory_order_relaxed);
  }
  bool session_running() const {
    return session_running_.load(std::memory_order_relaxed);
  }
  std::string runtime_name() const;

 private:
  friend struct OpenXrBackendTestAccess;
  struct ImageRecord {
    VkImage image = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkImageUsageFlags usage = 0;
    void* owner = nullptr;
    std::uint64_t order = 0;
  };
  struct EyeBinding {
    bool valid = false;
    std::uint64_t frame = 0;
    VkImage image = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkImageLayout final_layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    void* owner = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
  };
  struct EyeSwapchain {
    void* swapchain = nullptr;  // XrSwapchain (opaque to keep the header XR-free)
    std::vector<VkImage> images;
    std::vector<std::uint32_t> gl_images;
    std::uint32_t acquired_index = 0;
    bool image_acquired = false;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
  };

  bool InitializeSessionLocked(VkPhysicalDevice physical_device,
                               VkDevice device,
                               const VkDeviceCreateInfo* create_info,
                               std::string* error);
  // Session-owned controller action layer (poses, buttons, axes, haptics).
  // Created at instance level, attached per session, detached on teardown so
  // a replacement session never reuses stale spaces.
  XrActions actions_;
  bool actions_created_ = false;
  // Shared by action sync, snapshots, haptics and lifecycle operations.
  // Acquired after mutex_ on the render thread, but never held across
  // xrWaitFrame; the input thread takes only this controller lock.
  mutable std::mutex controller_mutex_;
  // Syncs actions once per frame at the predicted display time and folds the
  // resulting hand poses into published_pose_ before the guest renders.
  void SyncControllersLocked(std::uint64_t predicted_display_time);
  bool InitializeGlesSessionLocked(std::string *error);
  bool InitializeSessionResourcesLocked(std::string *error);
  bool CopyGlesEyesIntoSwapchains();
  void DisarmLocked();
  void* ResolveFunction(const char* name) const;
  void SetupDeviceAfterCreationLocked(VkPhysicalDevice physical_device,
                                      VkDevice device,
                                      const VkDeviceCreateInfo* create_info);
  void TeardownSessionLocked(const char* reason, bool preserve_device = false);
  void HandleSessionState(int state);
  void HandleReferenceSpaceChange(std::int64_t change_time);
  bool RecoverSessionLocked(std::uint64_t now_ns);
  void PollSessionEvents();
  // Drops the pose/images of the frame currently in flight without touching
  // guest ownership, so the next xrEndFrame submits no layer instead of an
  // image paired with a pose that no longer describes the same origin.
  void InvalidateInFlightPose(const char* reason);
  void RunFrameCycle(VkQueue queue, VkDevice device);
  bool CreateSwapchains(std::string* error);
  void DestroySwapchains();
  bool EnsureCopyResources(VkDevice device, std::uint32_t queue_family,
                           std::string* error);
  void DestroyCopyResources();
  // Records both eye copies (and, when the mirror is enabled, both staging
  // readbacks) into one command buffer, submits once and waits on one fence.
  // Replaces the previous per-eye submit+fence pair.
  bool CopyEyesIntoSwapchains(VkQueue queue);
  bool CaptureEyeEvidence(VkQueue queue, int eye);
  ScriptedPoseSample ComputeScriptedPose(std::uint64_t frame) const;
  void EnsureMirrorForExtent(std::uint32_t width, std::uint32_t height);
  MirrorVkProcs MirrorProcs() const;

  VrGraphicsApi graphics_api_ = VrGraphicsApi::kVulkan;
  GlesTransport *gles_ = nullptr;
  std::uint64_t gl_min_version_ = 0;
  std::uint64_t gl_max_version_ = 0;

  mutable std::mutex mutex_;
  std::atomic<bool> armed_{false};
  std::atomic<bool> recording_{false};
  std::atomic<bool> eyes_bound_{false};
  std::atomic<bool> session_running_{false};
  std::atomic<std::uint64_t> submitted_frames_{0};
  std::atomic<std::uint64_t> skipped_frames_{0};

  // XR handles are stored as opaque pointers in the header; the implementation
  // casts them back. This keeps vulkan-only consumers free of OpenXR headers.
  void* xr_instance_ = nullptr;
  std::uint64_t xr_system_ = 0;
  void* xr_session_ = nullptr;
  void* xr_local_space_ = nullptr;
  void* xr_view_space_ = nullptr;
  std::uint32_t xr_eye_width_[2] = {};
  std::uint32_t xr_eye_height_[2] = {};
  EyeSwapchain eye_swapchains_[2];
  int xr_blend_mode_ = 0;
  // Runtime name captured at xrCreateInstance so the bind-time diagnostics line
  // can report it without re-entering runtime_name() under the held mutex.
  std::string runtime_name_;

  VkInstance vk_instance_ = VK_NULL_HANDLE;
  VkDevice vk_device_ = VK_NULL_HANDLE;
  VkPhysicalDevice vk_physical_device_ = VK_NULL_HANDLE;
  std::uint32_t vk_queue_family_ = 0;
  void* vk_loader_ = nullptr;
  struct VkProcs;
  VkProcs* vk_ = nullptr;

  VkCommandPool copy_pool_ = VK_NULL_HANDLE;
  VkCommandBuffer copy_command_ = VK_NULL_HANDLE;
  VkFence copy_fence_ = VK_NULL_HANDLE;

  // Optional desktop two-eye mirror (MOCKTAIL_VR_MIRROR_SHM). Publishes the
  // latest complete pair through shared memory; inert when unset.
  VrMirror mirror_;

  std::unordered_map<VkImage, ImageRecord> images_;
  struct DesktopSwapchain {
    VkExtent2D extent;
    VkFormat format;
    std::vector<VkImage> images;
  };
  std::unordered_map<VkSwapchainKHR, DesktopSwapchain> desktop_swapchains_;
  std::unordered_map<VkImageView, VkImage> image_views_;
  std::unordered_map<VkFramebuffer, std::vector<VkImageView>> framebuffer_views_;
  std::unordered_map<VkFramebuffer, VkRenderPass> framebuffer_render_pass_;
  std::unordered_map<VkRenderPass, std::vector<VkImageLayout>> render_pass_final_layouts_;
  std::unordered_map<VkQueue, std::uint32_t> queue_families_;
  std::uint64_t image_order_ = 0;
  EyeBinding eyes_[2];

  // Frame cycle state (render thread only).
  bool frame_open_ = false;
  bool frame_should_render_ = false;
  // A running session is hidden in SYNCHRONIZED; shouldRender still governs rendering.
  bool visibility_lost_ = false;
  bool session_recovery_pending_ = false;
  bool test_session_loss_injected_ = false;
  std::uint64_t next_session_retry_ns_ = 0;
  std::uint64_t frame_display_time_ = 0;
  struct ViewState {
    float position[3];
    float orientation[4];
    float fov[4];  // angleLeft, angleRight, angleUp, angleDown (radians)
    bool valid;
  };
  ViewState frame_views_[2] = {};
  struct PoseStorage {
    float position[3] = {0.f, 0.f, 0.f};
    float orientation[4] = {0.f, 0.f, 0.f, 1.f};
  };
  PoseStorage frame_view_poses_[2];
  bool frame_views_valid_ = false;
  // One-shot guard so a canted-view runtime reports the rejection reason once
  // instead of once per frame per eye.
  bool canted_rejection_logged_ = false;
  mutable ScriptedPoseSample published_pose_;
  std::uint64_t xr_frame_counter_ = 0;
  std::unordered_map<void*, std::uint64_t> applied_poses_;
};

// Process-wide single backend owned by main. Adapter hooks resolve these
// exported C symbols; they are inert while no backend is armed.
OpenXrBackend* ActiveVrBackend();

}  // namespace mocktail::vr

#endif  // MOCKTAIL_VR_OPENXR_BACKEND_H_
