// Contract tests for the experimental DebugDeviceVR bridge. They pin the
// machine-code contracts of the exact 2998 payload ABI and the fail-closed
// bridge lifecycle; they do not execute any guest code.
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cmath>
#include <limits>
#include "mocktail/vr/openxr_backend.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "compat/build_profile.h"
#include "mocktail/vr/roblox_vr_device_bridge.h"
#include "vr/roblox_vr_device_bridge_internal.h"

namespace mocktail::vr {
namespace internal {
// Inject host fixture callbacks without executing or relaxing production ELF
// activation checks. Objects retain the guest byte layout, never host C++ ABI.
struct VrBridgeTestAccess {
  static void Configure(RobloxVrDeviceBridge &bridge, std::uintptr_t base,
                        std::uintptr_t size,
                        const compat::VrDebugDeviceBridgeProfile &profile,
                        RobloxVrDeviceBridge::StateGetterFn getter) {
    bridge.profile_ = profile;
    bridge.image_base_ = base;
    bridge.image_size_ = size;
    bridge.original_state_getter_ = getter;
    bridge.active_ = true;
  }
};
} // namespace internal
namespace {

using internal::kConstructorContractSize;
using internal::kEyeGetterContractSize;
using internal::kInitializerContractSize;
using internal::kStateGetterContractSize;

// Real 2998 machine code, transcribed from the verified payload disassembly
// (ade08266c67aee88ec9c1d00902150e1684dad3a).
const std::vector<uint8_t> kStateGetter = {
    0x55, 0x48, 0x89, 0xe5, 0x53, 0x50, 0x48, 0x89, 0xfb, 0x48, 0x83,
    0xc6, 0x14, 0xba, 0x38, 0x01, 0x00, 0x00, 0xe8, 0xd3, 0xb5, 0x36,
    0x03, 0x48, 0x89, 0xd8, 0x48, 0x83, 0xc4, 0x08, 0x5b, 0x5d, 0xc3};

const std::vector<uint8_t> kEyeGetter = {0x48, 0x63, 0xc6, 0x48, 0xc1, 0xe0,
                                         0x04, 0x48, 0x8b, 0x84, 0x07, 0x50,
                                         0x01, 0x00, 0x00, 0xc3};

std::vector<uint8_t> ValidConstructor() {
  std::vector<uint8_t> code(kConstructorContractSize, 0x90);
  const std::vector<uint8_t> prologue = {
      0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55,
      0x41, 0x54, 0x53, 0x48, 0x83, 0xec, 0x28, 0x49, 0x89, 0xcf,
      0x41, 0x89, 0xd4, 0x41, 0x89, 0xf5, 0x48, 0x89, 0xfb};
  std::memcpy(code.data(), prologue.data(), prologue.size());
  const std::vector<std::vector<uint8_t>> markers = {
      {0x48, 0x8d, 0x05, 0xf2, 0x74, 0x47, 0x03},
      {0x48, 0x89, 0x07},
      {0xc7, 0x47, 0x10, 0x06, 0x00, 0x00, 0x00},
      {0x4c, 0x8d, 0x77, 0x14, 0xba, 0x34, 0x01, 0x00, 0x00},
      {0x44, 0x89, 0xab, 0x90, 0x01, 0x00, 0x00, 0x44, 0x89, 0xa3, 0x94, 0x01,
       0x00, 0x00},
      {0x4c, 0x89, 0xbb, 0x98, 0x01, 0x00, 0x00},
  };
  size_t offset = 64;
  for (const auto &marker : markers) {
    std::memcpy(code.data() + offset, marker.data(), marker.size());
    offset += marker.size() + 8;
  }
  return code;
}

compat::VrDebugDeviceBridgeProfile TestProfile() {
  compat::VrDebugDeviceBridgeProfile profile{};
  profile.vtable_rva = 0x6c87b38;
  profile.constructor_rva = 0x3810614;
  profile.state_getter_rva = 0x38109f6;
  profile.eye_getter_rva = 0x38109e6;
  profile.eye_initializer_rva = 0x3810a18;
  profile.emulator_flag_storage_rva = 0x7369920;
  profile.device_create_framebuffer_vtable_offset = 0x180;
  return profile;
}

std::vector<uint8_t>
ValidInitializer(const compat::VrDebugDeviceBridgeProfile &profile) {
  std::vector<uint8_t> code(kInitializerContractSize, 0x90);
  const std::vector<uint8_t> prologue = {
      0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
      0x53, 0x48, 0x81, 0xec, 0xa8, 0x00, 0x00, 0x00, 0x48, 0x89, 0xb5, 0x50};
  std::memcpy(code.data(), prologue.data(), prologue.size());
  const uint32_t slot =
      static_cast<uint32_t>(profile.device_create_framebuffer_vtable_offset);
  const std::vector<std::vector<uint8_t>> markers = {
      {0xb1, 0x01, 0x88, 0x8f, 0x47, 0x01, 0x00, 0x00, 0x88, 0x8f,
       0x94, 0x00, 0x00, 0x00, 0x88, 0x8f, 0x44, 0x01, 0x00, 0x00},
      {0x48, 0x8d, 0x87, 0x70, 0x01, 0x00, 0x00},
      {0x48, 0x8d, 0x87, 0x50, 0x01, 0x00, 0x00},
      {0x48, 0x8b, 0x06, 0x48, 0x8b, 0x80, static_cast<uint8_t>(slot),
       static_cast<uint8_t>(slot >> 8), static_cast<uint8_t>(slot >> 16),
       static_cast<uint8_t>(slot >> 24)},
      {0x41, 0xbf, 0x01, 0x00, 0x00, 0x00, 0xf6, 0x85, 0x5c, 0xff, 0xff, 0xff,
       0x01, 0xb9, 0x00},
  };
  size_t offset = 64;
  for (const auto &marker : markers) {
    std::memcpy(code.data() + offset, marker.data(), marker.size());
    offset += marker.size() + 8;
  }
  return code;
}

compat::BuildProfile MakeBuildProfile(bool with_vr, bool allow_bridges) {
  compat::BuildProfile profile{};
  profile.version_name = "2.736.1408";
  profile.version_code = 2998;
  profile.elf_build_id = "ade08266c67aee88ec9c1d00902150e1684dad3a";
  profile.status = compat::BuildStatus::kSupported;
  profile.default_allowed = true;
  profile.allow_host_abi_bridges = allow_bridges;
  if (with_vr) {
    profile.vr_debug_device_bridge = TestProfile();
  }
  return profile;
}

struct alignas(8) GuestObject {
  std::array<std::uint8_t, 0x228> bytes{};
  template <typename T> void Put(std::size_t offset, T value) {
    std::memcpy(bytes.data() + offset, &value, sizeof(value));
  }
};
std::uint8_t fixture_flag = 1;
std::uintptr_t fixture_device_vtable[1];
int fixture_initializations = 0;
bool fixture_partial = false;
void FixtureInitialize(void *object, void *) {
  auto *guest = static_cast<GuestObject *>(object);
  ++fixture_initializations;
  guest->bytes[internal::kReadyByteOffset] = 1;
  guest->Put<std::uintptr_t>(internal::kFramebufferSlotOffset, 0x100);
  if (fixture_partial)
    return;
  guest->Put<std::uintptr_t>(internal::kFramebufferSlotOffset + 16, 0x200);
  guest->Put<std::uintptr_t>(internal::kTextureSlotOffset, 0x300);
  guest->Put<std::uintptr_t>(internal::kTextureSlotOffset + 16, 0x400);
}
void *FixtureState(void *output, void *object) {
  std::memcpy(output,
              static_cast<GuestObject *>(object)->bytes.data() +
                  internal::kStateOffset,
              internal::kStateCopySize);
  return output;
}

TEST(VrBridgeLifecycle, ReinitializesReusedAddressesAndMoreThanFourObjects) {
  RobloxVrDeviceBridge bridge;
  fixture_initializations = 0;
  fixture_partial = false;
  const auto init = reinterpret_cast<std::uintptr_t>(&FixtureInitialize);
  const auto flag = reinterpret_cast<std::uintptr_t>(&fixture_flag);
  const auto vtable = reinterpret_cast<std::uintptr_t>(fixture_device_vtable);
  const auto base = std::min({init, flag, vtable}) - 8;
  auto profile = TestProfile();
  profile.eye_initializer_rva = init - base;
  profile.emulator_flag_storage_rva = flag - base;
  profile.device_create_framebuffer_vtable_offset = 0;
  fixture_device_vtable[0] = init;
  std::uintptr_t device = vtable;
  internal::VrBridgeTestAccess::Configure(
      bridge, base, std::max({init, flag, vtable}) - base + 64, profile,
      &FixtureState);
  bridge.NoteHostPresent();
  std::array<std::uint8_t, internal::kStateCopySize> output{};
  std::array<GuestObject, 8> guests;
  for (int cycle = 0; cycle < 2; ++cycle) {
    for (auto &guest : guests) {
      guest = {}; // Constructor at a new or previously used address.
      guest.Put<std::uintptr_t>(0, base + profile.vtable_rva);
      guest.Put<const char *>(internal::kObjectNameOffset, "DebugDevice");
      guest.Put<std::uint32_t>(internal::kObjectTypeOffset, 6);
      guest.Put<std::uint32_t>(internal::kWidthOffset, 500);
      guest.Put<std::uint32_t>(internal::kHeightOffset, 500);
      guest.Put<void *>(internal::kGraphicsDeviceOffset, &device);
      const int before = fixture_initializations;
      bridge.OnStateGetterCall(output.data(), &guest);
      EXPECT_EQ(fixture_initializations, before + 1);
      EXPECT_EQ(output[internal::kStateReadyOffsetInCopy], 1);
      bridge.OnStateGetterCall(output.data(), &guest);
      EXPECT_EQ(fixture_initializations, before + 1);
    }
  }
  EXPECT_EQ(bridge.initialized_device_count(), 16);
  // A partially constructed device must not expose ready to the renderer or
  // repeatedly invoke the initializer on top of its existing allocations.
  auto &guest = guests[0];
  for (std::size_t i = internal::kFramebufferSlotOffset;
       i < internal::kWidthOffset; ++i)
    guest.bytes[i] = 0;
  guest.bytes[internal::kReadyByteOffset] = 0;
  fixture_partial = true;
  bridge.OnStateGetterCall(output.data(), &guest);
  EXPECT_EQ(output[internal::kStateReadyOffsetInCopy], 0);
  const int before = fixture_initializations;
  bridge.OnStateGetterCall(output.data(), &guest);
  EXPECT_EQ(fixture_initializations, before);
  EXPECT_EQ(output[internal::kStateReadyOffsetInCopy], 0);
  fixture_partial = false;
}

TEST(VrResourceReadiness,
     RejectsReadyByteAloneAliasedEyesAndInvalidDimensions) {
  GuestObject guest;
  guest.Put<std::uint32_t>(internal::kWidthOffset, 500);
  guest.Put<std::uint32_t>(internal::kHeightOffset, 500);
  EXPECT_EQ(internal::InspectEyeResources(&guest),
            internal::EyeResourceState::kEmpty);
  guest.bytes[internal::kReadyByteOffset] = 1;
  EXPECT_EQ(internal::InspectEyeResources(&guest),
            internal::EyeResourceState::kInvalid);
  FixtureInitialize(&guest, nullptr);
  EXPECT_EQ(internal::InspectEyeResources(&guest),
            internal::EyeResourceState::kReady);
  guest.Put<std::uintptr_t>(internal::kFramebufferSlotOffset + 16, 0x100);
  EXPECT_EQ(internal::InspectEyeResources(&guest),
            internal::EyeResourceState::kInvalid);
  FixtureInitialize(&guest, nullptr);
  guest.Put<std::uint32_t>(internal::kWidthOffset, 0);
  EXPECT_EQ(internal::InspectEyeResources(&guest),
            internal::EyeResourceState::kInvalid);
}

TEST(VrStateGetterContract, AcceptsExact2998Bytes) {
  ASSERT_EQ(kStateGetter.size(), kStateGetterContractSize);
  EXPECT_TRUE(internal::HasExpectedVrStateGetterContract(kStateGetter.data(),
                                                         kStateGetter.size()));
}

TEST(VrStateGetterContract, RejectsCorruptionAndShortInput) {
  EXPECT_FALSE(internal::HasExpectedVrStateGetterContract(nullptr, 0));
  EXPECT_FALSE(internal::HasExpectedVrStateGetterContract(
      kStateGetter.data(), kStateGetter.size() - 1));
  std::vector<uint8_t> corrupted = kStateGetter;
  // The sret copy size (0x138) must be pinned; corrupt one byte of it.
  corrupted[13] ^= 0xff;
  EXPECT_FALSE(internal::HasExpectedVrStateGetterContract(corrupted.data(),
                                                          corrupted.size()));
}

TEST(VrEyeGetterContract, AcceptsExact2998Bytes) {
  ASSERT_EQ(kEyeGetter.size(), kEyeGetterContractSize);
  EXPECT_TRUE(internal::HasExpectedVrEyeGetterContract(kEyeGetter.data(),
                                                       kEyeGetter.size()));
}

TEST(VrEyeGetterContract, RejectsDifferentEyeSlotOffset) {
  std::vector<uint8_t> shifted = kEyeGetter;
  // +0x150 -> +0x160: a different eye slot layout must not pass.
  shifted[11] = 0x60;
  EXPECT_FALSE(
      internal::HasExpectedVrEyeGetterContract(shifted.data(), shifted.size()));
}

TEST(VrConstructorContract, AcceptsMarkersAndRejectsEachMissingOne) {
  const std::vector<uint8_t> valid = ValidConstructor();
  EXPECT_TRUE(
      internal::HasExpectedVrConstructorContract(valid.data(), valid.size()));
  EXPECT_FALSE(internal::HasExpectedVrConstructorContract(valid.data(),
                                                          valid.size() - 1));
  // Corrupting the graphics-device capture (+0x198 store) must fail closed.
  std::vector<uint8_t> no_device_store = valid;
  for (size_t index = 0; index + 7 <= no_device_store.size(); ++index) {
    if (no_device_store[index] == 0x4c && no_device_store[index + 1] == 0x89 &&
        no_device_store[index + 2] == 0xbb) {
      no_device_store[index + 3] ^= 0xff;
      break;
    }
  }
  EXPECT_FALSE(internal::HasExpectedVrConstructorContract(
      no_device_store.data(), no_device_store.size()));
}

TEST(VrInitializerContract, PinsDeviceVtableSlotFromProfile) {
  const compat::VrDebugDeviceBridgeProfile profile = TestProfile();
  const std::vector<uint8_t> valid = ValidInitializer(profile);
  EXPECT_TRUE(internal::HasExpectedVrEyeInitializerContract(
      valid.data(), valid.size(), profile));

  // The 3092 payload uses +0x188; a profile mismatch must fail closed.
  compat::VrDebugDeviceBridgeProfile other = profile;
  other.device_create_framebuffer_vtable_offset = 0x188;
  EXPECT_FALSE(internal::HasExpectedVrEyeInitializerContract(
      valid.data(), valid.size(), other));

  std::vector<uint8_t> no_ready_byte = valid;
  for (size_t index = 0; index + 8 <= no_ready_byte.size(); ++index) {
    if (no_ready_byte[index] == 0xb1 && no_ready_byte[index + 1] == 0x01 &&
        no_ready_byte[index + 2] == 0x88) {
      no_ready_byte[index + 4] ^= 0xff;
      break;
    }
  }
  EXPECT_FALSE(internal::HasExpectedVrEyeInitializerContract(
      no_ready_byte.data(), no_ready_byte.size(), profile));
}

TEST(VrVtableContract, RequiresExactProfileRvas) {
  const compat::VrDebugDeviceBridgeProfile profile = TestProfile();
  const std::uintptr_t base = 0x10000000;
  std::vector<std::uintptr_t> vtable(internal::kEyeInitializerVtableSlot / 8 +
                                     1);
  vtable[internal::kStateGetterVtableSlot / 8] =
      base + profile.state_getter_rva;
  vtable[internal::kEyeGetterVtableSlot / 8] = base + profile.eye_getter_rva;
  vtable[internal::kEyeInitializerVtableSlot / 8] =
      base + profile.eye_initializer_rva;
  EXPECT_TRUE(
      internal::HasExpectedVrDebugDeviceVtable(vtable.data(), base, profile));
  vtable[internal::kEyeInitializerVtableSlot / 8] += 16;
  EXPECT_FALSE(
      internal::HasExpectedVrDebugDeviceVtable(vtable.data(), base, profile));
}

TEST(VrBridgeLifecycle, InstallRejectsMissingMetadataAndWrongPermissions) {
  {
    RobloxVrDeviceBridge bridge;
    const compat::BuildProfile profile = MakeBuildProfile(false, true);
    const Status status = bridge.Install(profile);
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), StatusCode::kFailedPrecondition);
  }
  {
    RobloxVrDeviceBridge bridge;
    const compat::BuildProfile profile = MakeBuildProfile(true, false);
    const Status status = bridge.Install(profile);
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), StatusCode::kFailedPrecondition);
  }
}

TEST(VrBridgeLifecycle, SingleProcessOwnerAndFailClosedActivation) {
  RobloxVrDeviceBridge first;
  ASSERT_TRUE(first.Install(MakeBuildProfile(true, true)).ok());
  {
    RobloxVrDeviceBridge second;
    const Status status = second.Install(MakeBuildProfile(true, true));
    EXPECT_FALSE(status.ok());
    EXPECT_EQ(status.code(), StatusCode::kFailedPrecondition);
  }
  // A non-ELF image base must be rejected without touching memory.
  const Status bad_image = first.Activate(0);
  EXPECT_FALSE(bad_image.ok());
  EXPECT_EQ(bad_image.code(), StatusCode::kInvalidArgument);
  EXPECT_FALSE(first.active());
  // While armed, a failed activation must surface to the caller.
  const Status notified = NotifyRobloxVrImageLoaded(0);
  EXPECT_FALSE(notified.ok());
  first.Shutdown();
}

TEST(VrBridgeLifecycle, NotificationIsNoOpWithoutArmedBridge) {
  EXPECT_TRUE(NotifyRobloxVrImageLoaded(0).ok());
}

TEST(VrProfileParsing, ParsesAndRejectsBridgeSection) {
  const std::string directory = std::string(MOCKTAIL_TEST_BINARY_DIRECTORY);
  const auto write_manifest = [&directory](const char *name,
                                           const std::string &contents) {
    const std::string path = directory + "/" + name;
    std::ofstream output(path);
    output << contents;
    output.close();
    return path;
  };
  const std::string base_profile = R"({
    "schema_version": 1,
    "profiles": [{
      "version_name": "2.736.1408",
      "version_code": 2998,
      "elf_build_id": "ade08266c67aee88ec9c1d00902150e1684dad3a",
      "status": "supported",
      "default_allowed": true,
      "allow_legacy_binary_patches": false,
      "allow_host_abi_bridges": true,
      "allow_host_constructor_replay": true,
      "reason": "test"
    }]
  })";
  {
    const std::string path =
        write_manifest("vr_profile_absent.json", base_profile);
    const auto result = compat::FindBuildProfile(
        path, "ade08266c67aee88ec9c1d00902150e1684dad3a");
    ASSERT_TRUE(result);
    ASSERT_TRUE(result.profile.has_value());
    EXPECT_FALSE(result.profile->vr_debug_device_bridge.has_value());
    std::remove(path.c_str());
  }
  {
    std::string manifest = base_profile;
    const std::string anchor = R"("reason": "test")";
    manifest.replace(manifest.find(anchor), anchor.size(),
                     R"("vr_debug_device_bridge": {
             "vtable_rva": "0x6c87b38",
             "constructor_rva": "0x3810614",
             "state_getter_rva": "0x38109f6",
             "eye_getter_rva": "0x38109e6",
             "eye_initializer_rva": "0x3810a18",
             "emulator_flag_storage_rva": "0x7369920",
             "device_create_framebuffer_vtable_offset": "0x180"
           },
           "reason": "test")");
    const std::string path = write_manifest("vr_profile_valid.json", manifest);
    const auto result = compat::FindBuildProfile(
        path, "ade08266c67aee88ec9c1d00902150e1684dad3a");
    ASSERT_TRUE(result);
    ASSERT_TRUE(result.profile.has_value());
    ASSERT_TRUE(result.profile->vr_debug_device_bridge.has_value());
    const auto &bridge = *result.profile->vr_debug_device_bridge;
    EXPECT_EQ(bridge.vtable_rva, 0x6c87b38u);
    EXPECT_EQ(bridge.eye_initializer_rva, 0x3810a18u);
    EXPECT_EQ(bridge.device_create_framebuffer_vtable_offset, 0x180u);
    std::remove(path.c_str());
  }
  {
    std::string manifest = base_profile;
    const std::string anchor = R"("reason": "test")";
    manifest.replace(manifest.find(anchor), anchor.size(),
                     R"("vr_debug_device_bridge": {
                          "vtable_rva": "0x6c87b38"
                        },
                        "reason": "test")");
    const std::string path =
        write_manifest("vr_profile_incomplete.json", manifest);
    const auto result = compat::FindBuildProfile(
        path, "ade08266c67aee88ec9c1d00902150e1684dad3a");
    EXPECT_FALSE(result);
    std::remove(path.c_str());
  }
}

// Reproduces issue #153: a Roblox build whose compatibility profile arms the
// fullscreen and output-device bridges but carries no vr_debug_device_bridge
// metadata must fail closed when --vr is requested, surfacing the exact
// FATAL message the reporter saw.
TEST(VrProfileParsing, Issue153VrBridgeFailsWithoutMetadataForArmedProfile) {
  const std::string directory = std::string(MOCKTAIL_TEST_BINARY_DIRECTORY);
  const auto write_manifest = [&directory](const char *name,
                                           const std::string &contents) {
    const std::string path = directory + "/" + name;
    std::ofstream output(path);
    output << contents;
    output.close();
    return path;
  };

  // The reporter's payload Build ID from the issue log. The profile is
  // fully armed for fullscreen + FMOD output-device interposition, so the
  // runtime reaches the VR device bridge install step, but it omits
  // vr_debug_device_bridge entirely.
  const std::string manifest = R"({
    "schema_version": 1,
    "profiles": [{
      "version_name": "2.738.266",
      "version_code": 3092,
      "elf_build_id": "5f0704edd9064f566ee3d6df2bd2fabbcc709f03",
      "status": "supported",
      "default_allowed": true,
      "allow_legacy_binary_patches": false,
      "allow_host_abi_bridges": true,
      "allow_host_constructor_replay": true,
      "user_game_settings_fullscreen_setter_rva": "0x4612345",
      "fmod_output_device_bridge": {
        "vtable_rva": "0x6d00040",
        "string_constructor_rva": "0x1d40000",
        "count_method_rva": "0x32d1000",
        "info_method_rva": "0x32d10a0",
        "current_method_rva": "0x32d1050",
        "select_method_rva": "0x32d0d04"
      },
      "reason": "issue 153 reproduction profile: armed without vr metadata"
    }]
  })";

  const std::string path =
      write_manifest("vr_profile_issue153.json", manifest);
  const auto result = compat::FindBuildProfile(
      path, "5f0704edd9064f566ee3d6df2bd2fabbcc709f03");
  std::remove(path.c_str());

  // The profile must resolve: fullscreen and output-device bridges would arm.
  ASSERT_TRUE(result);
  ASSERT_TRUE(result.profile.has_value());
  ASSERT_TRUE(result.profile->fmod_output_device_bridge.has_value());
  ASSERT_TRUE(result.profile->user_game_settings_fullscreen_setter_rva
                  .has_value());
  // But it carries no VR debug-device bridge metadata.
  ASSERT_FALSE(result.profile->vr_debug_device_bridge.has_value());

  // Installing the VR device bridge against this profile must fail closed with
  // the exact message the reporter hit in the FATAL log line.
  RobloxVrDeviceBridge bridge;
  const Status status = bridge.Install(*result.profile);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.code(), StatusCode::kFailedPrecondition);
  EXPECT_EQ(status.message(),
            "experimental Roblox VR requires vr_debug_device_bridge metadata in "
            "the exact-build compatibility profile");
}

} // namespace
} // namespace mocktail::vr

namespace mocktail::vr {
TEST(VrPoseBridge, ConvertsRuntimePoseAndAsymmetricProjectionWithoutChangingReadiness) {
  std::array<std::uint8_t, internal::kStateCopySize> state;
  state.fill(0xa5);
  ScriptedPoseSample pose;
  pose.valid = true;
  pose.position[0] = 0.4f;
  pose.position[1] = 1.6f;
  pose.orientation[1] = std::sin(0.2f);
  pose.orientation[3] = std::cos(0.2f);
  for (int eye = 0; eye < 2; ++eye) {
    pose.eye_offset[eye][0] = eye ? 0.032f : -0.032f;
    const float fov[] = {-0.7f, 0.8f, 0.9f, -0.6f};
    std::copy_n(fov, 4, pose.eye_fov[eye]);
  }
  ASSERT_TRUE(internal::ApplyXrPose(state.data(), pose));
  EXPECT_EQ(state[0], 1);
  EXPECT_EQ(state[0x40], 0);
  EXPECT_EQ(state[internal::kStateReadyOffsetInCopy], 0xa5);
  float position[3], orientation[4], left[3], right[3], projection[4];
  std::memcpy(position, state.data()+4, 12);
  std::memcpy(orientation, state.data()+0x10, 16);
  std::memcpy(left, state.data()+0xf8, 12);
  std::memcpy(right, state.data()+0x104, 12);
  std::memcpy(projection, state.data()+0x110, 16);
  EXPECT_FLOAT_EQ(position[0], 0.4f);  // no double metre-to-stud conversion
  EXPECT_FLOAT_EQ(position[1], 1.6f);
  EXPECT_FLOAT_EQ(orientation[1], std::sin(0.2f));
  EXPECT_FLOAT_EQ(left[0], -0.032f);
  EXPECT_FLOAT_EQ(right[0], 0.032f);
  EXPECT_FLOAT_EQ(projection[0], std::tan(0.9f));  // up/down/left/right
  EXPECT_FLOAT_EQ(projection[1], std::tan(0.6f));
  EXPECT_FLOAT_EQ(projection[2], std::tan(0.7f));
  EXPECT_FLOAT_EQ(projection[3], std::tan(0.8f));
}
TEST(VrPoseBridge, InvalidTrackingAndNonfiniteValuesNeverBecomeValid) {
  std::array<std::uint8_t, internal::kStateCopySize> state{};
  state[0] = state[internal::kStateReadyOffsetInCopy] = 1;
  ScriptedPoseSample pose;
  EXPECT_FALSE(internal::ApplyXrPose(state.data(), pose));
  EXPECT_EQ(state[0], 0);
  EXPECT_EQ(state[internal::kStateReadyOffsetInCopy], 1);
  pose.valid = true;
  pose.position[2] = std::numeric_limits<float>::quiet_NaN();
  EXPECT_FALSE(internal::ApplyXrPose(state.data(), pose));
  EXPECT_EQ(state[0], 0);
  pose.position[2] = 0;
  pose.orientation[3] = 0;
  EXPECT_FALSE(internal::ApplyXrPose(state.data(), pose));
}
}  // namespace mocktail::vr

namespace mocktail::vr {
TEST(VrHandPoseBridge, ValidGripPosesFillBothHandRecordsInMetres) {
  std::array<std::uint8_t, internal::kStateCopySize> state;
  state.fill(0xa5);
  ScriptedPoseSample pose;
  pose.valid = true;
  pose.orientation[3] = 1.f;
  pose.hands[0].grip_valid = true;
  pose.hands[0].grip_position[0] = -0.25f;
  pose.hands[0].grip_position[1] = 1.0f;
  pose.hands[0].grip_orientation[1] = std::sin(0.3f);
  pose.hands[0].grip_orientation[3] = std::cos(0.3f);
  pose.hands[1].grip_valid = true;
  pose.hands[1].grip_position[0] = 0.25f;
  pose.hands[1].grip_orientation[3] = 1.f;
  ASSERT_TRUE(internal::ApplyXrPose(state.data(), pose));
  // Left hand record at copy+0x40 (UserCFrame index 1), right at +0x60 (2).
  EXPECT_EQ(state[internal::kLeftHandRecordOffset], 1);
  EXPECT_EQ(state[internal::kRightHandRecordOffset], 1);
  // The unused extra record (index 3) must stay invalid.
  EXPECT_EQ(state[internal::kExtraRecordOffset], 0);
  float left_pos[3], left_ori[4], right_pos[3];
  std::memcpy(left_pos, state.data() + internal::kLeftHandRecordOffset +
                            internal::kRecordPositionOffset, 12);
  std::memcpy(left_ori, state.data() + internal::kLeftHandRecordOffset +
                            internal::kRecordOrientationOffset, 16);
  std::memcpy(right_pos, state.data() + internal::kRightHandRecordOffset +
                             internal::kRecordPositionOffset, 12);
  // Metres only: the guest's 0x37eacb8 applies the 10/3 stud conversion.
  EXPECT_FLOAT_EQ(left_pos[0], -0.25f);
  EXPECT_FLOAT_EQ(left_pos[1], 1.0f);
  EXPECT_FLOAT_EQ(left_ori[1], std::sin(0.3f));
  EXPECT_FLOAT_EQ(left_ori[3], std::cos(0.3f));
  EXPECT_FLOAT_EQ(right_pos[0], 0.25f);
}

TEST(VrHandPoseBridge, UntrackedOrNonFiniteHandsStayInvalidWithoutTouchingHead) {
  std::array<std::uint8_t, internal::kStateCopySize> state;
  state.fill(0);
  ScriptedPoseSample pose;
  pose.valid = true;
  pose.orientation[3] = 1.f;
  // Left hand untracked (grip_valid false), right hand has a NaN quaternion.
  pose.hands[0].grip_valid = false;
  pose.hands[1].grip_valid = true;
  pose.hands[1].grip_orientation[0] = std::numeric_limits<float>::quiet_NaN();
  ASSERT_TRUE(internal::ApplyXrPose(state.data(), pose));
  EXPECT_EQ(state[0], 1);  // head still applied
  EXPECT_EQ(state[internal::kLeftHandRecordOffset], 0);
  EXPECT_EQ(state[internal::kRightHandRecordOffset], 0);
  // A non-normalized quaternion must also be rejected.
  pose.hands[1].grip_orientation[0] = 0.f;
  pose.hands[1].grip_orientation[3] = 0.5f;
  ASSERT_TRUE(internal::ApplyXrPose(state.data(), pose));
  EXPECT_EQ(state[internal::kRightHandRecordOffset], 0);
}

TEST(VrHandPoseBridge, LosingTrackingClearsHandValidByteNotWholeRecord) {
  std::array<std::uint8_t, internal::kStateCopySize> state;
  state.fill(0);
  ScriptedPoseSample pose;
  pose.valid = true;
  pose.orientation[3] = 1.f;
  pose.hands[0].grip_valid = true;
  pose.hands[0].grip_orientation[3] = 1.f;
  ASSERT_TRUE(internal::ApplyXrPose(state.data(), pose));
  ASSERT_EQ(state[internal::kLeftHandRecordOffset], 1);
  // Tracking lost: valid byte clears so the guest reports untracked; the stale
  // pose bytes remain but are unreachable while valid==0 (no teleport to origin).
  pose.hands[0].grip_valid = false;
  ASSERT_TRUE(internal::ApplyXrPose(state.data(), pose));
  EXPECT_EQ(state[internal::kLeftHandRecordOffset], 0);
  float stale[3];
  std::memcpy(stale, state.data() + internal::kLeftHandRecordOffset +
                        internal::kRecordPositionOffset, 12);
  EXPECT_FLOAT_EQ(stale[0], 0.f);  // position was never written for this hand
}
}  // namespace mocktail::vr

namespace mocktail::vr {
TEST(VrNativeController, StateCopyPublishesChannelsAndHapticsWithoutChangingReadiness) {
  std::array<std::uint8_t, internal::kStateCopySize> state{};
  state[0x133] = 77;
  ScriptedPoseSample pose;
  pose.valid = pose.controllers_connected = true;
  pose.controller_channels[3] = .7f;
  pose.controller_channels[11] = -1.f;
  pose.controller_channels[24] = 1.f;
  ASSERT_TRUE(internal::ApplyXrPose(state.data(), pose));
  EXPECT_EQ(state[0x80], 1);
  EXPECT_EQ(state[0x132], 1);
  EXPECT_EQ(state[0x133], 77);
  float trigger, y, x;
  std::memcpy(&trigger, state.data() + 0x88 + 3*4, 4);
  std::memcpy(&y, state.data() + 0x88 + 11*4, 4);
  std::memcpy(&x, state.data() + 0x88 + 24*4, 4);
  EXPECT_FLOAT_EQ(trigger, .7f);
  EXPECT_FLOAT_EQ(y, -1.f);
  EXPECT_FLOAT_EQ(x, 1.f);
  pose.valid = false;
  EXPECT_FALSE(internal::ApplyXrPose(state.data(), pose));
  EXPECT_EQ(state[0x80], 0);
  EXPECT_EQ(state[0x132], 0);
  std::memcpy(&trigger, state.data() + 0x88 + 3*4, 4);
  EXPECT_FLOAT_EQ(trigger, 0);
}
TEST(VrAimPointer, CorrectsOnlyPointerAndAppliesTranslationScaleOnce) {
  float frame[12] = {1,0,0, 0,1,0, 0,0,1, 10,20,30};
  VrHandPose hand;
  hand.grip_valid = hand.aim_valid = true;
  hand.aim_position[0] = .3f;
  hand.aim_orientation[1] = std::sin(.3f);
  hand.aim_orientation[3] = std::cos(.3f);
  ASSERT_TRUE(internal::ApplyAimToWorldFrame(frame, hand, 2, 0));
  EXPECT_NEAR(frame[9], 12, 1e-5);
  EXPECT_FLOAT_EQ(frame[10], 20);
  EXPECT_NEAR(frame[0], std::cos(.6f), 1e-5);
  EXPECT_NEAR(frame[2], std::sin(.6f), 1e-5);
  EXPECT_FLOAT_EQ(hand.grip_orientation[3], 1);
}
TEST(VrAimPointer, CancelsNativeHandPitchWithoutChangingWorldOrigin) {
  const float a = -.3490658504f;
  float frame[12] = {1,0,0, 0,std::cos(a),-std::sin(a), 0,std::sin(a),std::cos(a), 10,20,30};
  VrHandPose hand;
  hand.grip_valid = hand.aim_valid = true;
  ASSERT_TRUE(internal::ApplyAimToWorldFrame(frame, hand, 1, -20));
  EXPECT_NEAR(frame[0], 1, 1e-5);
  EXPECT_NEAR(frame[5], 0, 1e-5);
  EXPECT_NEAR(frame[8], 1, 1e-5);
  EXPECT_FLOAT_EQ(frame[9], 10);
}
TEST(VrAimPointer, InvalidAimOrScaleKeepsOriginalFrame) {
  const std::array<float,12> original{1,0,0, 0,1,0, 0,0,1, 10,20,30};
  auto frame = original;
  VrHandPose hand;
  EXPECT_FALSE(internal::ApplyAimToWorldFrame(frame.data(), hand, 1, 0));
  EXPECT_EQ(frame, original);
  hand.grip_valid = hand.aim_valid = true;
  EXPECT_FALSE(internal::ApplyAimToWorldFrame(frame.data(), hand, -1, 0));
  hand.aim_orientation[0] = std::numeric_limits<float>::quiet_NaN();
  EXPECT_FALSE(internal::ApplyAimToWorldFrame(frame.data(), hand, 1, 0));
  EXPECT_EQ(frame, original);
}
}
