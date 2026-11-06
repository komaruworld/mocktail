#include "compat/build_profile.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <unistd.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
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

TEST(BuildProfileTest, Payload2908IsSupportedWithoutBinaryPatches) {
  const ProfileLookupResult result = FindBuildProfile(
      kManifestPath, "63c5109637b7d7b2bdb8ed8f858023ff5ef49326");

  ASSERT_TRUE(result) << result.error;
  ASSERT_TRUE(result.profile.has_value());
  EXPECT_EQ(result.profile->version_name, "2.734.917");
  EXPECT_EQ(result.profile->version_code, 2908);
  EXPECT_EQ(result.profile->status, BuildStatus::kSupported);
  EXPECT_TRUE(result.profile->default_allowed);
  EXPECT_FALSE(result.profile->allow_legacy_binary_patches);
  EXPECT_TRUE(result.profile->allow_host_abi_bridges);
  EXPECT_TRUE(result.profile->allow_host_constructor_replay);
  ASSERT_TRUE(
      result.profile->user_game_settings_fullscreen_setter_rva.has_value());
  EXPECT_EQ(*result.profile->user_game_settings_fullscreen_setter_rva,
            0x44fee64U);
  ASSERT_TRUE(result.profile->fmod_output_device_bridge.has_value());
  EXPECT_EQ(result.profile->fmod_output_device_bridge->vtable_rva,
            0x6b7eee8U);
  EXPECT_EQ(result.profile->fmod_output_device_bridge->string_constructor_rva,
            0x1cfd5ecU);
  EXPECT_EQ(result.profile->fmod_output_device_bridge->count_method_rva,
            0x32736d2U);
  EXPECT_EQ(result.profile->fmod_output_device_bridge->info_method_rva,
            0x3273772U);
  EXPECT_EQ(result.profile->fmod_output_device_bridge->current_method_rva,
            0x3273722U);
  EXPECT_EQ(result.profile->fmod_output_device_bridge->select_method_rva,
            0x32734a6U);
}

TEST(BuildProfileTest, ReportsUnknownBuildWithoutInventingProfile) {
  const ProfileLookupResult result = FindBuildProfile(
      kManifestPath, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");

  ASSERT_TRUE(result) << result.error;
  EXPECT_FALSE(result.profile.has_value());
}

TEST(BuildProfileTest, Payload2998SupportsFullscreenAndHostAudioByDefault) {
  const ProfileLookupResult result = FindBuildProfile(
      kManifestPath, "ade08266c67aee88ec9c1d00902150e1684dad3a");

  ASSERT_TRUE(result) << result.error;
  ASSERT_TRUE(result.profile.has_value());
  EXPECT_EQ(result.profile->version_name, "2.736.1408");
  EXPECT_EQ(result.profile->version_code, 2998);
  EXPECT_EQ(result.profile->status, BuildStatus::kSupported);
  EXPECT_TRUE(result.profile->default_allowed);
  EXPECT_FALSE(result.profile->allow_legacy_binary_patches);
  EXPECT_TRUE(result.profile->allow_host_abi_bridges);
  EXPECT_TRUE(result.profile->allow_host_constructor_replay);
  ASSERT_TRUE(
      result.profile->user_game_settings_fullscreen_setter_rva.has_value());
  EXPECT_EQ(*result.profile->user_game_settings_fullscreen_setter_rva,
            0x45ad8aaU);
  ASSERT_TRUE(result.profile->fmod_output_device_bridge.has_value());
  EXPECT_EQ(result.profile->fmod_output_device_bridge->vtable_rva,
            0x6c58040U);
  EXPECT_EQ(result.profile->fmod_output_device_bridge->string_constructor_rva,
            0x1d32df8U);
  EXPECT_EQ(result.profile->fmod_output_device_bridge->count_method_rva,
            0x32d0ee0U);
  EXPECT_EQ(result.profile->fmod_output_device_bridge->info_method_rva,
            0x32d0f80U);
  EXPECT_EQ(result.profile->fmod_output_device_bridge->current_method_rva,
            0x32d0f30U);
  EXPECT_EQ(result.profile->fmod_output_device_bridge->select_method_rva,
            0x32d0cb4U);
}

TEST(BuildProfileTest, RejectsInvalidBuildId) {
  const ProfileLookupResult result = FindBuildProfile(kManifestPath, "oops");

  EXPECT_FALSE(result);
  EXPECT_FALSE(result.error.empty());
}

TEST(BuildProfileTest, Payload3222PublishesInputRoutesInItsManifest) {
  constexpr const char* build_id = "a67a91ef96c700c0730acd36a250c2f93cfafbc1";
  nlohmann::json document;
  std::ifstream(kManifestPath) >> document;
  const auto& profiles = document.at("profiles");
  const auto entry = std::find_if(profiles.begin(), profiles.end(),
      [&](const auto& profile) { return profile.at("elf_build_id") == build_id; });
  ASSERT_NE(entry, profiles.end());
  const auto& bridge = entry->at("fmod_output_device_bridge");
  for (const auto* field : {"input_count_method_rva", "input_info_method_rva",
                            "input_current_method_rva", "input_select_method_rva"}) {
    EXPECT_TRUE(bridge.contains(field)) << field;
  }

  const auto result = FindBuildProfile(kManifestPath, build_id);
  ASSERT_TRUE(result && result.profile) << result.error;
  ASSERT_TRUE(result.profile->fmod_output_device_bridge);
  const auto& parsed = *result.profile->fmod_output_device_bridge;
  EXPECT_TRUE(parsed.has_input_devices());
  EXPECT_EQ(parsed.input_method_rvas,
            (std::array<std::uintptr_t, 4>{0x2ee4f10, 0x2ee4f1e,
                                          0x2ee536c, 0x2ee3f32}));
}

TEST(BuildProfileTest, RestoresCached3222InputOnlyForTheExactVtableLayout) {
  constexpr const char* build_id = "a67a91ef96c700c0730acd36a250c2f93cfafbc1";
  nlohmann::json document;
  std::ifstream(kManifestPath) >> document;
  const auto& profiles = document.at("profiles");
  const auto found = std::find_if(profiles.begin(), profiles.end(),
      [&](const auto& profile) { return profile.at("elf_build_id") == build_id; });
  ASSERT_NE(found, profiles.end());
  auto cached = *found;
  for (const auto* field : {"input_count_method_rva", "input_info_method_rva",
                            "input_current_method_rva", "input_select_method_rva"}) {
    cached["fmod_output_device_bridge"].erase(field);
  }
  cached["status"] = "experimental";
  cached["default_allowed"] = false;
  char path[] = "/tmp/mocktail-input-3222-XXXXXX";
  const int descriptor = mkstemp(path);
  ASSERT_GE(descriptor, 0);
  close(descriptor);
  struct Cleanup {
    const char* path;
    ~Cleanup() { unlink(path); }
  } cleanup{path};
  const auto read = [&](const nlohmann::json& profile) {
    {
      std::ofstream output(path);
      output << nlohmann::json{{"schema_version", 1}, {"profiles", {profile}}};
    }
    return FindBuildProfile(path, profile.at("elf_build_id").get<std::string>());
  };

  const auto restored = read(cached);
  ASSERT_TRUE(restored && restored.profile) << restored.error;
  ASSERT_TRUE(restored.profile->fmod_output_device_bridge);
  EXPECT_TRUE(restored.profile->fmod_output_device_bridge->has_input_devices());
  EXPECT_EQ(restored.profile->fmod_output_device_bridge->input_method_rvas,
            (std::array<std::uintptr_t, 4>{0x2ee4f10, 0x2ee4f1e,
                                          0x2ee536c, 0x2ee3f32}));
  EXPECT_EQ(restored.profile->status, BuildStatus::kExperimental);
  EXPECT_FALSE(restored.profile->default_allowed);

  for (int changed = 0; changed < 3; ++changed) {
    SCOPED_TRACE(changed);
    auto other = cached;
    if (changed == 0) other["elf_build_id"] = std::string(40, 'a');
    if (changed == 1) other["fmod_output_device_bridge"]["vtable_rva"] = "0x1000";
    if (changed == 2) other["fmod_output_device_bridge"]["vtable_layout_version"] = 1;
    const auto result = read(other);
    ASSERT_TRUE(result && result.profile) << result.error;
    ASSERT_TRUE(result.profile->fmod_output_device_bridge);
    EXPECT_FALSE(result.profile->fmod_output_device_bridge->has_input_devices());
  }

  auto partial = cached;
  partial["fmod_output_device_bridge"]["input_count_method_rva"] = "0x2ee4f10";
  EXPECT_FALSE(read(partial));
}

TEST(BuildProfileTest, RejectsInvalidManifest) {
  const ProfileLookupResult result = FindBuildProfile(
      "/dev/null", "d0cb1fa0deb3d9161b4cd77530cbcd2e50de3a21");

  EXPECT_FALSE(result);
  EXPECT_FALSE(result.error.empty());
}

TEST(BuildProfileTest, RejectsPartialInputProfilesAndUpgradesCachedLayouts) {
  nlohmann::json document;
  {
    std::ifstream input(kManifestPath);
    input >> document;
  }
  auto &profiles = document["profiles"];
  const auto found = std::find_if(
      profiles.begin(), profiles.end(), [](const auto &profile) {
        return profile.value("elf_build_id", "") ==
               "ade08266c67aee88ec9c1d00902150e1684dad3a";
      });
  ASSERT_NE(found, profiles.end());
  auto &entry = *found;
  auto &bridge = entry["fmod_output_device_bridge"];
  char path[] = "/tmp/mocktail-input-profile-XXXXXX";
  int fd = mkstemp(path);
  ASSERT_GE(fd, 0);
  close(fd);
  const auto read = [&] {
    {
      std::ofstream output(path);
      output << document;
    }
    return FindBuildProfile(path, entry["elf_build_id"].get<std::string>());
  };
  bridge.erase("input_count_method_rva");
  EXPECT_FALSE(read());
  for (const auto *name : {"input_info_method_rva", "input_current_method_rva",
                           "input_select_method_rva"})
    bridge.erase(name);
  auto result = read();
  ASSERT_TRUE(result && result.profile);
  EXPECT_TRUE(result.profile->fmod_output_device_bridge->has_input_devices());
  EXPECT_EQ(result.profile->fmod_output_device_bridge->input_method_rvas[0],
            0x32d12dcU);
  entry["elf_build_id"] = "5f0704edd9064f566ee3d6df2bd2fabbcc709f03";
  bridge["vtable_rva"] = "0x6cd3ce0";
  bridge["vtable_layout_version"] = 2;
  result = read();
  ASSERT_TRUE(result && result.profile);
  EXPECT_EQ(result.profile->fmod_output_device_bridge->input_method_rvas[3],
            0x320bd18U);
  bridge["vtable_rva"] = "0x1000";
  result = read();
  ASSERT_TRUE(result && result.profile);
  EXPECT_FALSE(result.profile->fmod_output_device_bridge->has_input_devices());
  unlink(path);
}

TEST(BuildProfileTest, ParsesOnlyKnownFmodVtableLayouts) {
  std::ifstream input(kManifestPath);
  const std::string manifest{std::istreambuf_iterator<char>(input), {}};
  const std::string marker = "\"fmod_output_device_bridge\": {";
  const auto position = manifest.find(marker);
  ASSERT_NE(position, std::string::npos);
  char path[] = "/tmp/mocktail-fmod-profile-XXXXXX";
  const int descriptor = mkstemp(path);
  ASSERT_GE(descriptor, 0);
  close(descriptor);
  for (const std::string layout : {"1", "2", "0", "3", "true", "\"2\"", "null"}) {
    auto document = manifest;
    document.insert(position + marker.size(),
                    "\"vtable_layout_version\":" + layout + ",");
    { std::ofstream output(path); output << document; }
    const auto result = FindBuildProfile(
        path, "d0cb1fa0deb3d9161b4cd77530cbcd2e50de3a21");
    if (layout == "1" || layout == "2") {
      EXPECT_TRUE(result) << result.error;
      if (result.profile && result.profile->fmod_output_device_bridge) {
        const auto& bridge = *result.profile->fmod_output_device_bridge;
        EXPECT_EQ(bridge.current_vtable_index(), layout == "2" ? 8U : 7U);
        EXPECT_EQ(bridge.select_vtable_index(), layout == "2" ? 19U : 17U);
      } else {
        ADD_FAILURE() << "parsed layout lost its FMOD profile";
      }
    } else {
      EXPECT_FALSE(result) << layout;
    }
  }
  unlink(path);
}

}  // namespace
}  // namespace mocktail::compat
