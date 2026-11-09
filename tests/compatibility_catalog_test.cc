#include "update/compatibility_catalog.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

#include "compat/guest_abi.h"

namespace mocktail::update {
namespace {

class CompatibilityCatalogTest : public testing::Test {
 protected:
  void SetUp() override {
    char pattern[] = "/tmp/mocktail_catalog_XXXXXX";
    const char* created = mkdtemp(pattern);
    ASSERT_NE(created, nullptr);
    directory = created;
  }

  void TearDown() override {
    std::error_code ignored;
    if (!directory.empty()) std::filesystem::remove_all(directory, ignored);
  }

  nlohmann::json ValidProfile() {
    return {{"status", "supported"},
            {"default_allowed", true},
            {"allow_legacy_binary_patches", false},
            {"abi", compat::kGuestAbi},
            {"version_name", "1.2.3"},
            {"version_code", 123},
            {"elf_build_id", std::string(40, 'a')}};
  }

  CompatibilityCatalogResult Load(const nlohmann::json& document) {
    const auto path = directory / "catalog.json";
    std::ofstream(path) << document.dump();
    return LoadCompatibilityCatalog(path);
  }

  std::filesystem::path directory;
};

TEST_F(CompatibilityCatalogTest, LoadsValidSupportedProfile) {
  const auto catalog =
      Load({{"schema_version", 1}, {"profiles", {ValidProfile()}}});
  ASSERT_TRUE(catalog) << catalog.error;
  ASSERT_EQ(catalog.profiles.size(), 1u);
  EXPECT_EQ(catalog.profiles.front().version_code, 123u);
}

TEST_F(CompatibilityCatalogTest, RejectsInvalidSchemaTypesWithoutTerminating) {
  for (const auto& version :
       nlohmann::json::array({nullptr, true, "1", 1.0, 4294967297ULL,
                              nlohmann::json::object()})) {
    SCOPED_TRACE(version.dump());
    const auto catalog =
        Load({{"schema_version", version}, {"profiles", {ValidProfile()}}});
    EXPECT_FALSE(catalog);
    EXPECT_FALSE(catalog.error.empty());
  }
}

TEST_F(CompatibilityCatalogTest, RejectsInvalidPolicyTypesWithoutTerminating) {
  const std::pair<const char*, nlohmann::json> invalid[] = {
      {"status", true},
      {"status", 1},
      {"status", nullptr},
      {"default_allowed", "true"},
      {"default_allowed", 1},
      {"default_allowed", nullptr},
      {"allow_legacy_binary_patches", "false"},
      {"allow_legacy_binary_patches", 0},
      {"allow_legacy_binary_patches", nullptr}};
  for (const auto& [name, value] : invalid) {
    SCOPED_TRACE(name);
    SCOPED_TRACE(value.dump());
    auto profile = ValidProfile();
    profile[name] = value;
    const auto catalog = Load({{"schema_version", 1}, {"profiles", {profile}}});
    EXPECT_FALSE(catalog);
    EXPECT_FALSE(catalog.error.empty());
  }
}

TEST_F(CompatibilityCatalogTest, MissingPolicyFieldsCannotAuthorizeAPayload) {
  for (const char* name :
       {"status", "default_allowed", "allow_legacy_binary_patches"}) {
    SCOPED_TRACE(name);
    auto profile = ValidProfile();
    profile.erase(name);
    const auto catalog = Load({{"schema_version", 1}, {"profiles", {profile}}});
    EXPECT_FALSE(catalog);
    EXPECT_TRUE(catalog.profiles.empty());
  }
}

TEST_F(CompatibilityCatalogTest, FiltersOtherArchitectures) {
  auto other = ValidProfile();
  other["abi"] = compat::kGuestAbi == "x86_64" ? "arm64-v8a" : "x86_64";
  other["version_code"] = 456;
  const auto catalog =
      Load({{"schema_version", 1}, {"profiles", {other, ValidProfile()}}});
  ASSERT_TRUE(catalog) << catalog.error;
  ASSERT_EQ(catalog.profiles.size(), 1u);
  EXPECT_EQ(catalog.profiles.front().version_code, 123u);
}

}  // namespace
}  // namespace mocktail::update
