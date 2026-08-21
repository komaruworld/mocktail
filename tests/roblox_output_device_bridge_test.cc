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
