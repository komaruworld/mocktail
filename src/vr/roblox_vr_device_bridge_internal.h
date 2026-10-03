#ifndef MOCKTAIL_VR_ROBLOX_VR_DEVICE_BRIDGE_INTERNAL_H_
#define MOCKTAIL_VR_ROBLOX_VR_DEVICE_BRIDGE_INTERNAL_H_

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "compat/build_profile.h"
#include "mocktail/vr/xr_controller.h"

namespace mocktail::vr { struct ScriptedPoseSample; }
namespace mocktail::vr::internal {

struct VrBuildContract {
  std::string_view build_id;
  compat::VrDebugDeviceBridgeProfile device;
  std::uintptr_t state_memcpy_rva;
  std::uintptr_t pointer_frame_rva;
  std::uintptr_t workspace_getter_rva;
  std::uintptr_t camera_vtable_offset;
  std::uintptr_t hand_pitch_storage_rva;
  std::uintptr_t haptic_sink_rva;
};

const VrBuildContract* FindVrBuildContract(std::string_view build_id);

// Writes only a caller-owned state copy; preserves all resource readiness.
bool ApplyXrPose(void* state, const ScriptedPoseSample& pose);
// Adjust a world-space grip CoordinateFrame to OpenXR aim without changing
// the engine's hand pose. Native frame layout: row-major R[9], position[3].
bool ApplyAimToWorldFrame(float* frame, const VrHandPose& hand,
                         float head_scale, float native_hand_pitch_degrees);

// Guest ABI layout constants of RBX::Graphics::DebugDeviceVR. They are pinned
// per exact build by the machine-code contracts below; changing payload
// requires re-deriving both.
inline constexpr std::size_t kObjectNameOffset = 0x8;
inline constexpr std::size_t kObjectTypeOffset = 0x10;
inline constexpr std::uint32_t kExpectedObjectType = 6;
inline constexpr std::size_t kStateOffset = 0x14;
inline constexpr std::size_t kStateCopySize = 0x138;
inline constexpr std::size_t kStateReadyOffsetInCopy = 0x133;
// Per-record pose layout inside the 0x138 state copy. Verified against 2998
// consumer 0x28a6242 (docs/vr-agent-evidence/controller-2998): four 0x20-byte
// records are each converted by helper 0x37eacb8 (metres->studs x10/3, quat
// normalized) and stored via setUserCFrame 0x4d56906 at VRService+0x174+idx*0x30.
//   copy+0x00 head      -> UserCFrame index 0
//   copy+0x20 extra     -> index 3
//   copy+0x40 left hand -> index 1
//   copy+0x60 right hand-> index 2
// Record field layout (same for head and hands): valid byte +0, position
// (3 floats, metres) +4, quaternion xyzw (4 floats) +0x10. The guest applies
// the 10/3 conversion and a -20 deg hand pitch offset itself; neither is
// pre-applied here.
inline constexpr std::size_t kPoseRecordStride = 0x20;
inline constexpr std::size_t kHeadRecordOffset = 0x00;
inline constexpr std::size_t kExtraRecordOffset = 0x20;
inline constexpr std::size_t kLeftHandRecordOffset = 0x40;
inline constexpr std::size_t kRightHandRecordOffset = 0x60;
inline constexpr std::size_t kRecordValidOffset = 0x00;
inline constexpr std::size_t kRecordPositionOffset = 0x04;
inline constexpr std::size_t kRecordOrientationOffset = 0x10;
inline constexpr std::size_t kReadyByteOffset = 0x147;
inline constexpr std::size_t kFramebufferSlotOffset = 0x150;
inline constexpr std::size_t kTextureSlotOffset = 0x170;
inline constexpr std::size_t kEyeSlotStride = 0x10;
inline constexpr std::size_t kWidthOffset = 0x190;
inline constexpr std::size_t kHeightOffset = 0x194;
inline constexpr std::size_t kGraphicsDeviceOffset = 0x198;
// Per-eye camera parameters filled by the constructor: symmetric +-0.25 stud
// view offsets (half IPD), tan-space projection values and FOV degrees.
inline constexpr std::size_t kEye0ViewOffset = 0x10c;
inline constexpr std::size_t kEye1ViewOffset = 0x118;
inline constexpr std::size_t kEye0ProjectionOffset = 0x124;
inline constexpr std::size_t kEye1ProjectionOffset = 0x134;
inline constexpr std::size_t kFieldOfViewOffset = 0x1a0;
inline constexpr std::size_t kHalfInterpupillaryOffset = 0x1b8;

inline constexpr std::size_t kStateGetterVtableSlot = 0x28;
inline constexpr std::size_t kEyeGetterVtableSlot = 0x48;
inline constexpr std::size_t kEyeInitializerVtableSlot = 0xd0;

inline constexpr std::size_t kStateGetterContractSize = 33;
inline constexpr std::size_t kEyeGetterContractSize = 16;
inline constexpr std::size_t kConstructorContractSize = 664;
inline constexpr std::size_t kInitializerContractSize = 700;

enum class EyeResourceState { kEmpty, kReady, kInvalid };
// Reads only the current live object's ABI bytes. A ready byte alone is
// insufficient; dimensions and both distinct framebuffer/texture slots count.
EyeResourceState InspectEyeResources(const void *object);

// Exact 33-byte state getter: sret ABI (rdi = hidden 0x138-byte result
// buffer, rsi = device), copies state from self+0x14 and returns the buffer.
bool HasExpectedVrStateGetterContract(const std::uint8_t* code,
                                      std::size_t size,
                                      const VrBuildContract& contract);

// Exact 16-byte eye framebuffer getter: self + 0x150 + eyeIndex * 16.
bool HasExpectedVrEyeGetterContract(const std::uint8_t *code, std::size_t size);

// Constructor: captures rcx (graphics device) into self+0x198, width/height
// into +0x190/+0x194, type dword 6 into +0x10, and clears the state block.
bool HasExpectedVrConstructorContract(
    const std::uint8_t* code, std::size_t size,
    const compat::VrDebugDeviceBridgeProfile& profile);

bool HasExpectedVrPointerContract(const std::uint8_t* code, std::size_t size,
                                  const VrBuildContract& contract);

// Eye-resource initializer: raises ready byte self+0x147 plus +0x94/+0x144
// and creates the per-eye resources through the graphics-device vtable slot
// taken from the profile (2998 uses +0x180; other builds differ).
bool HasExpectedVrEyeInitializerContract(
    const std::uint8_t *code, std::size_t size,
    const compat::VrDebugDeviceBridgeProfile &profile);

// DebugDeviceVR vtable must point the three interposed/used slots at the
// exact profile RVAs before any interposition.
bool HasExpectedVrDebugDeviceVtable(
    const std::uintptr_t *vtable, std::uintptr_t image_base,
    const compat::VrDebugDeviceBridgeProfile &profile);

} // namespace mocktail::vr::internal

#endif // MOCKTAIL_VR_ROBLOX_VR_DEVICE_BRIDGE_INTERNAL_H_
