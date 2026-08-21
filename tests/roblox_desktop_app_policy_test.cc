#include "runtime/roblox_desktop_app_policy.h"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>

namespace mocktail {
namespace runtime {
namespace {

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    char pattern[] = "/tmp/mocktail_desktop_policy_XXXXXX";
    char* created = mkdtemp(pattern);
    if (created != nullptr) {
      path_ = created;
    }
  }
  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

bool WriteJson(const std::filesystem::path& path, const nlohmann::json& value) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) {
    return false;
  }
  std::ofstream output(path);
  output << value.dump();
  return output.good();
}

nlohmann::json ReadJson(const std::filesystem::path& path) {
  std::ifstream input(path);
  return nlohmann::json::parse(input, nullptr, false, true);
}

nlohmann::json DecodeConfigurations(const nlohmann::json& storage) {
  return nlohmann::json::parse(
      storage["AppConfiguration"].get_ref<const std::string&>(), nullptr, false,
      true);
}

nlohmann::json DecodePolicy(const nlohmann::json& configurations,
                            const std::string& key) {
  return nlohmann::json::parse(
      configurations[key].get_ref<const std::string&>(), nullptr, false, true);
}

mode_t FileMode(const std::filesystem::path& path) {
  struct stat status{};
  return stat(path.c_str(), &status) == 0 ? status.st_mode & 0777 : 0;
}

void ExpectDesktopLayout(const nlohmann::json& policy) {
  EXPECT_EQ(policy["PlatformGroup"], "Desktop");
  EXPECT_EQ(policy["UseGridHomePage"], true);
  EXPECT_EQ(policy["UseGridPageLayout"], true);
  EXPECT_EQ(policy["SystemBarPlacement"], "Left");
  EXPECT_EQ(policy["ShouldSystemBarUsuallyBePresent"], true);
  EXPECT_EQ(policy["DevicePreferencesPersistentPresenceVariant"], "windows");
}

TEST(RobloxDesktopAppPolicyTest,
     NormalizesLayoutWithoutChangingServerEntitlements) {
  TemporaryDirectory temporary;
  ASSERT_FALSE(temporary.path().empty());
  const std::filesystem::path storage_path =
      temporary.path() / "appData/LocalStorage/appStorage.json";
  const std::filesystem::path default_path =
      temporary.path() / "assets/GuacDefaultPolicy-GlobalDist.json";
  const nlohmann::json original_policy = {
      {"PlatformGroup", "Unknown"},       {"UseGridHomePage", nullptr},
      {"SystemBarPlacement", "Bottom"},   {"EligibleForVideoCapture", false},
      {"AccountOwnedMarker", "preserve"},
  };
  const nlohmann::json configurations = {
      {"GUAC:42:app-policy", original_policy.dump()},
  };
  ASSERT_TRUE(
      WriteJson(storage_path, {{"AppConfiguration", configurations.dump()},
                               {"UnrelatedPreference", "keep"}}));
  ASSERT_TRUE(WriteJson(default_path, {{"PlatformGroup", "Unknown"}}));

  const DesktopAppPolicyResult result =
      ApplyDesktopAppPolicy(storage_path, default_path, 42);
  ASSERT_TRUE(result) << result.error;
  EXPECT_TRUE(result.updated);
  EXPECT_EQ(result.normalized_policy_count, 1U);
  EXPECT_EQ(FileMode(storage_path), 0600);
  ExpectDesktopLayout(nlohmann::json::parse(result.policy_json));

  const nlohmann::json storage = ReadJson(storage_path);
  EXPECT_EQ(storage["UnrelatedPreference"], "keep");
  const nlohmann::json updated = DecodeConfigurations(storage);
  ASSERT_TRUE(updated.contains("GUAC:-1:app-policy"));
  ASSERT_TRUE(updated.contains("GUAC:42:app-policy"));
  const nlohmann::json user_policy =
      DecodePolicy(updated, "GUAC:42:app-policy");
  ExpectDesktopLayout(user_policy);
  EXPECT_EQ(user_policy["EligibleForVideoCapture"], false);
  EXPECT_EQ(user_policy["AccountOwnedMarker"], "preserve");
  ExpectDesktopLayout(DecodePolicy(updated, "GUAC:-1:app-policy"));

  const DesktopAppPolicyResult repeated =
      ApplyDesktopAppPolicy(storage_path, default_path, 42);
  ASSERT_TRUE(repeated) << repeated.error;
  EXPECT_FALSE(repeated.updated);
  EXPECT_EQ(repeated.normalized_policy_count, 2U);
}

TEST(RobloxDesktopAppPolicyTest, SeedsMissingStorageFromPayloadDefault) {
  TemporaryDirectory temporary;
  const std::filesystem::path storage_path =
      temporary.path() / "appData/LocalStorage/appStorage.json";
  const std::filesystem::path default_path =
      temporary.path() / "assets/GuacDefaultPolicy-GlobalDist.json";
  ASSERT_TRUE(WriteJson(default_path, {{"PlatformGroup", "Unknown"},
                                       {"PayloadDefaultMarker", 7}}));

  const DesktopAppPolicyResult result =
      ApplyDesktopAppPolicy(storage_path, default_path, -1);
  ASSERT_TRUE(result) << result.error;
  EXPECT_TRUE(result.app_storage_created);
  EXPECT_TRUE(result.updated);

  const nlohmann::json configurations =
      DecodeConfigurations(ReadJson(storage_path));
  ASSERT_EQ(configurations.size(), 1U);
  const nlohmann::json policy =
      DecodePolicy(configurations, "GUAC:-1:app-policy");
  ExpectDesktopLayout(policy);
  EXPECT_EQ(policy["PayloadDefaultMarker"], 7);
  EXPECT_EQ(nlohmann::json::parse(result.policy_json)["PayloadDefaultMarker"],
            7);
}

TEST(RobloxDesktopAppPolicyTest,
     SelectsAuthenticatedEntitlementsAndSynchronizesCacheKeys) {
  TemporaryDirectory temporary;
  const std::filesystem::path storage_path =
      temporary.path() / "appData/LocalStorage/appStorage.json";
  const std::filesystem::path default_path = temporary.path() / "default.json";
  const nlohmann::json guest_policy = {{"PlatformGroup", "Unknown"},
                                       {"Entitlement", "guest"}};
  const nlohmann::json user_policy = {{"PlatformGroup", "Unknown"},
                                      {"Entitlement", "authenticated"}};
  const nlohmann::json configurations = {
      {"GUAC:-1:app-policy", guest_policy.dump()},
      {"GUAC:42:app-policy", user_policy.dump()},
  };
  ASSERT_TRUE(
      WriteJson(storage_path, {{"AppConfiguration", configurations.dump()}}));
  ASSERT_TRUE(WriteJson(default_path, {{"PlatformGroup", "Unknown"}}));

  const DesktopAppPolicyResult result =
      ApplyDesktopAppPolicy(storage_path, default_path, 42);
  ASSERT_TRUE(result) << result.error;
  const nlohmann::json selected = nlohmann::json::parse(result.policy_json);
  ExpectDesktopLayout(selected);
  EXPECT_EQ(selected["Entitlement"], "authenticated");

  const nlohmann::json updated = DecodeConfigurations(ReadJson(storage_path));
  EXPECT_EQ(DecodePolicy(updated, "GUAC:-1:app-policy"), selected);
  EXPECT_EQ(DecodePolicy(updated, "GUAC:42:app-policy"), selected);
}

TEST(RobloxDesktopAppPolicyTest,
     MergesRuntimeOverrideWithoutDiscardingOtherClientSettings) {
  const std::string policy =
      nlohmann::json({{"PlatformGroup", "Desktop"}}).dump();
  std::string merged;
  std::string error;
  ASSERT_TRUE(MergeDesktopAppPolicyClientSettingsOverride(
      policy, R"({"DFIntTaskSchedulerTargetFps":"240"})", &merged, &error))
      << error;
  const nlohmann::json overrides = nlohmann::json::parse(merged);
  EXPECT_EQ(overrides["DFIntTaskSchedulerTargetFps"], "240");
  EXPECT_EQ(overrides["FStringAppConfigurationOverrideAppPolicy"], policy);

  EXPECT_FALSE(MergeDesktopAppPolicyClientSettingsOverride(
      policy, R"({"FStringAppConfigurationOverrideAppPolicy":"different"})",
      &merged, &error));
  EXPECT_NE(error.find("conflicts"), std::string::npos);
  EXPECT_FALSE(MergeDesktopAppPolicyClientSettingsOverride("not-json", "{}",
                                                           &merged, &error));
}

TEST(RobloxDesktopAppPolicyTest, RejectsMalformedOrSymlinkedStorage) {
  TemporaryDirectory temporary;
  const std::filesystem::path default_path = temporary.path() / "default.json";
  ASSERT_TRUE(WriteJson(default_path, {{"PlatformGroup", "Unknown"}}));
  const std::filesystem::path malformed = temporary.path() / "malformed.json";
  {
    std::ofstream output(malformed);
    output << "not-json";
  }
  EXPECT_FALSE(ApplyDesktopAppPolicy(malformed, default_path, 42));

  const std::filesystem::path outside = temporary.path() / "outside.json";
  const std::filesystem::path linked = temporary.path() / "linked.json";
  ASSERT_TRUE(WriteJson(outside, {{"Preserve", true}}));
  ASSERT_EQ(symlink(outside.c_str(), linked.c_str()), 0);
  EXPECT_FALSE(ApplyDesktopAppPolicy(linked, default_path, 42));
  EXPECT_EQ(ReadJson(outside)["Preserve"], true);
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
