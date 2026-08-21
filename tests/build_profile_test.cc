#include "compat/build_profile.h"

#include <gtest/gtest.h>

#include <string>

#ifndef MOCKTAIL_TEST_SOURCE_DIR
#error "MOCKTAIL_TEST_SOURCE_DIR must point at the Mocktail source tree"
#endif

namespace mocktail::compat {
namespace {

const std::string kManifestPath =
    std::string(MOCKTAIL_TEST_SOURCE_DIR) + "/config/roblox_compatibility.json";

TEST(BuildProfileTest, FindsCurrentPayloadAsSupported) {
  const ProfileLookupResult result = FindBuildProfile(
      kManifestPath, "d0cb1fa0deb3d9161b4cd77530cbcd2e50de3a21");

  ASSERT_TRUE(result) << result.error;
  ASSERT_TRUE(result.profile.has_value());
  EXPECT_EQ(result.profile->version_name, "2.725.1142");
  EXPECT_EQ(result.profile->status, BuildStatus::kSupported);
  EXPECT_TRUE(result.profile->default_allowed);
  EXPECT_FALSE(result.profile->allow_legacy_binary_patches);
  EXPECT_TRUE(result.profile->allow_host_abi_bridges);
  EXPECT_TRUE(result.profile->allow_host_constructor_replay);
  ASSERT_TRUE(
      result.profile->user_game_settings_fullscreen_setter_rva.has_value());
  EXPECT_EQ(*result.profile->user_game_settings_fullscreen_setter_rva,
            0x4095564U);
  ASSERT_TRUE(result.profile->fmod_output_device_bridge.has_value());
  EXPECT_EQ(result.profile->fmod_output_device_bridge->vtable_rva, 0x6742678U);
  EXPECT_EQ(result.profile->fmod_output_device_bridge->string_constructor_rva,
            0x1bf898cU);
  EXPECT_EQ(result.profile->fmod_output_device_bridge->count_method_rva,
            0x2c2a082U);
  EXPECT_EQ(result.profile->fmod_output_device_bridge->info_method_rva,
            0x2c2a122U);
  EXPECT_EQ(result.profile->fmod_output_device_bridge->current_method_rva,
            0x2c2a0d2U);
  EXPECT_EQ(result.profile->fmod_output_device_bridge->select_method_rva,
            0x2c29e56U);
}

TEST(BuildProfileTest, Payload2628IsSupportedWithoutBinaryPatches) {
  const ProfileLookupResult result = FindBuildProfile(
      kManifestPath, "1686400865ae0e408cd7bd67de7a439625c6fd13");

  ASSERT_TRUE(result) << result.error;
  ASSERT_TRUE(result.profile.has_value());
  EXPECT_EQ(result.profile->version_name, "2.727.1199");
  EXPECT_EQ(result.profile->version_code, 2628);
  EXPECT_EQ(result.profile->status, BuildStatus::kSupported);
  EXPECT_TRUE(result.profile->default_allowed);
  EXPECT_FALSE(result.profile->allow_legacy_binary_patches);
  EXPECT_TRUE(result.profile->allow_host_abi_bridges);
  EXPECT_TRUE(result.profile->allow_host_constructor_replay);
  EXPECT_FALSE(
      result.profile->user_game_settings_fullscreen_setter_rva.has_value());
  EXPECT_FALSE(result.profile->fmod_output_device_bridge.has_value());
}

TEST(BuildProfileTest, ReportsUnknownBuildWithoutInventingProfile) {
  const ProfileLookupResult result = FindBuildProfile(
      kManifestPath, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");

  ASSERT_TRUE(result) << result.error;
  EXPECT_FALSE(result.profile.has_value());
}

TEST(BuildProfileTest, RejectsInvalidBuildId) {
  const ProfileLookupResult result = FindBuildProfile(kManifestPath, "oops");

  EXPECT_FALSE(result);
  EXPECT_FALSE(result.error.empty());
}

TEST(BuildProfileTest, RejectsInvalidManifest) {
  const ProfileLookupResult result = FindBuildProfile(
      "/dev/null", "d0cb1fa0deb3d9161b4cd77530cbcd2e50de3a21");

  EXPECT_FALSE(result);
  EXPECT_FALSE(result.error.empty());
}

}  // namespace
}  // namespace mocktail::compat
