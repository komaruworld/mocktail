#include "mocktail/audio/roblox_output_device_bridge.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string>

#include "audio/roblox_output_device_bridge_internal.h"

namespace mocktail::audio {
namespace {

compat::FmodOutputDeviceBridgeProfile TestProfile() {
  return compat::FmodOutputDeviceBridgeProfile{0x1000, 0x2000, 0x3000,
                                               0x4000, 0x5000, 0x6000};
}

TEST(RobloxOutputDeviceBridgeTest, ValidatesExactGuestStringAbiContract) {
  constexpr std::array<std::uint8_t, 24> kExpected = {
      0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x54, 0x53, 0x48,
      0x89, 0xd3, 0x49, 0x89, 0xf6, 0x49, 0x89, 0xff, 0x48, 0x83, 0xfa, 0x16,
  };
  EXPECT_TRUE(internal::HasExpectedFmodStringConstructorContract(
      kExpected.data(), kExpected.size()));

  auto changed = kExpected;
  changed.back() = 0x17;
  EXPECT_FALSE(internal::HasExpectedFmodStringConstructorContract(
      changed.data(), changed.size()));
  EXPECT_FALSE(internal::HasExpectedFmodStringConstructorContract(
      kExpected.data(), kExpected.size() - 1));
}

TEST(RobloxOutputDeviceBridgeTest, ValidatesEveryInterposedVtableSlot) {
  constexpr std::uintptr_t kImageBase = 0x10000000;
  const compat::FmodOutputDeviceBridgeProfile profile = TestProfile();
  std::array<std::uintptr_t, 18> vtable{};
  vtable[5] = kImageBase + profile.count_method_rva;
  vtable[6] = kImageBase + profile.info_method_rva;
  vtable[7] = kImageBase + profile.current_method_rva;
  vtable[17] = kImageBase + profile.select_method_rva;
  EXPECT_TRUE(internal::HasExpectedFmodOutputDeviceVtable(vtable.data(),
                                                          kImageBase, profile));

  ++vtable[17];
  EXPECT_FALSE(internal::HasExpectedFmodOutputDeviceVtable(
      vtable.data(), kImageBase, profile));
}

TEST(RobloxOutputDeviceBridgeTest, Validates3092LayoutWithoutUsingListSlots) {
  constexpr std::uintptr_t kBase = 0x10000000;
  auto profile = TestProfile();
  profile.vtable_layout_version = 2;
  std::array<std::uintptr_t, 20> vtable{};
  vtable[5] = kBase + profile.count_method_rva;
  vtable[6] = kBase + profile.info_method_rva;
  vtable[7] = kBase + 0x7000;
  vtable[8] = kBase + profile.current_method_rva;
  vtable[17] = kBase + 0x8000;
  vtable[19] = kBase + profile.select_method_rva;
  EXPECT_TRUE(internal::HasExpectedFmodOutputDeviceVtable(vtable.data(), kBase,
                                                          profile));
  profile.vtable_layout_version = 1;
  EXPECT_FALSE(internal::HasExpectedFmodOutputDeviceVtable(vtable.data(), kBase,
                                                           profile));
  profile.vtable_layout_version = 3;
  EXPECT_FALSE(internal::HasExpectedFmodOutputDeviceVtable(vtable.data(), kBase,
                                                           profile));
}

TEST(RobloxOutputDeviceBridgeTest, PinsCaptureToBuildIdVtableAndLayout) {
  compat::BuildProfile profile;
  profile.allow_host_abi_bridges = true;
  profile.elf_build_id = "5f0704edd9064f566ee3d6df2bd2fabbcc709f03";
  EXPECT_EQ(internal::FindFmodInputCaptureProfile(profile), nullptr);
  profile.fmod_output_device_bridge = TestProfile();
  EXPECT_EQ(internal::FindFmodInputCaptureProfile(profile), nullptr);
  profile.fmod_output_device_bridge->vtable_rva = 0x6cd3ce0;
  EXPECT_EQ(internal::FindFmodInputCaptureProfile(profile), nullptr);
  profile.fmod_output_device_bridge->vtable_layout_version = 2;
  ASSERT_NE(internal::FindFmodInputCaptureProfile(profile), nullptr);
  profile.allow_host_abi_bridges = false;
  EXPECT_EQ(internal::FindFmodInputCaptureProfile(profile), nullptr);
  profile.allow_host_abi_bridges = true;
  profile.elf_build_id = "unknown";
  EXPECT_EQ(internal::FindFmodInputCaptureProfile(profile), nullptr);
}

TEST(RobloxOutputDeviceBridgeTest, ValidatesEveryCaptureSlotOnBothBuilds) {
  constexpr std::uintptr_t kBase = 0x10000000;
  for (int version : {2998, 3092}) {
    compat::BuildProfile profile;
    profile.allow_host_abi_bridges = true;
    profile.elf_build_id = version == 2998
                               ? "ade08266c67aee88ec9c1d00902150e1684dad3a"
                               : "5f0704edd9064f566ee3d6df2bd2fabbcc709f03";
    profile.fmod_output_device_bridge = TestProfile();
    profile.fmod_output_device_bridge->vtable_rva =
        version == 2998 ? 0x6c58040 : 0x6cd3ce0;
    profile.fmod_output_device_bridge->vtable_layout_version =
        version == 2998 ? 1 : 2;
    const auto* capture = internal::FindFmodInputCaptureProfile(profile);
    ASSERT_NE(capture, nullptr);
    const std::array<std::size_t, 6> expected_slots =
        version == 2998 ? std::array<std::size_t, 6>{4, 11, 12, 13, 14, 15}
                        : std::array<std::size_t, 6>{4, 13, 14, 15, 16, 17};
    EXPECT_EQ(capture->slots, expected_slots);
    std::array<std::uintptr_t, 20> vtable{};
    for (std::size_t i = 0; i < capture->slots.size(); ++i) {
      vtable[capture->slots[i]] = kBase + capture->method_rvas[i];
    }
    EXPECT_TRUE(internal::HasExpectedFmodInputCaptureVtable(vtable.data(),
                                                            kBase, *capture));
    for (const auto slot : capture->slots) {
      ++vtable[slot];
      EXPECT_FALSE(internal::HasExpectedFmodInputCaptureVtable(vtable.data(),
                                                               kBase, *capture))
          << slot;
      --vtable[slot];
    }
    EXPECT_FALSE(
        internal::HasExpectedFmodInputCaptureVtable(nullptr, kBase, *capture));
    EXPECT_FALSE(internal::HasExpectedFmodInputCaptureVtable(vtable.data(), 0,
                                                             *capture));
    EXPECT_FALSE(internal::HasExpectedFmodInputCaptureVtable(
        vtable.data(), UINTPTR_MAX, *capture));
  }
}

TEST(RobloxOutputDeviceBridgeTest, BuildsStableDistinctHostGuids) {
  const std::string first = internal::MakeOutputDeviceGuid(17, "USB Headset");
  EXPECT_EQ(first, internal::MakeOutputDeviceGuid(17, "USB Headset"));
  EXPECT_NE(first, internal::MakeOutputDeviceGuid(18, "USB Headset"));
  EXPECT_NE(first, internal::MakeOutputDeviceGuid(17, "HDMI Output"));
  EXPECT_EQ(internal::MakeOutputDeviceGuid(0, "ignored"), "mocktail:default");
}

TEST(RobloxOutputDeviceBridgeTest, EnforcesSingleProcessOwner) {
  compat::BuildProfile profile;
  profile.elf_build_id = "d0cb1fa0deb3d9161b4cd77530cbcd2e50de3a21";
  profile.allow_host_abi_bridges = true;
  profile.fmod_output_device_bridge = TestProfile();

  RobloxOutputDeviceBridge first;
  RobloxOutputDeviceBridge second;
  ASSERT_TRUE(first.Install(profile).ok());
  EXPECT_EQ(second.Install(profile).code(), StatusCode::kFailedPrecondition);
  first.Shutdown();
  EXPECT_TRUE(second.Install(profile).ok());
  second.Shutdown();
}

}  // namespace
}  // namespace mocktail::audio
