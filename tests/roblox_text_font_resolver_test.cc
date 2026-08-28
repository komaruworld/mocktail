#include "runtime/roblox_text_font_resolver.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace mocktail {
namespace runtime {
namespace {

class TemporaryFontAssets final {
 public:
  TemporaryFontAssets() {
    char pattern[] = "/tmp/mocktail-font-assets-XXXXXX";
    const char* created = mkdtemp(pattern);
    if (created != nullptr) {
      root_ = created;
    }
  }

  ~TemporaryFontAssets() {
    if (!root_.empty()) {
      std::error_code error;
      std::filesystem::remove_all(root_, error);
    }
  }

  const std::filesystem::path& root() const { return root_; }
  std::filesystem::path content() const { return root_ / "assets/content"; }

  bool CreateFile(const std::filesystem::path& relative,
                  const std::string& contents = {}) const {
    const std::filesystem::path path = root_ / relative;
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) {
      return false;
    }
    std::ofstream output(path, std::ios::binary);
    output << contents;
    return output.good();
  }

 private:
  std::filesystem::path root_;
};

TEST(RobloxTextFontResolverTest, UsesExactApkMappingAndSizeRatio) {
  TemporaryFontAssets assets;
  ASSERT_FALSE(assets.root().empty());
  ASSERT_TRUE(assets.CreateFile(
      "assets/android/fonts/font-mappings.json",
      R"([{"enum":13,"font":"PressStart2P-Regular.ttf","fromRbxFontRatio":0.9761678384}])"));
  ASSERT_TRUE(
      assets.CreateFile("assets/content/fonts/PressStart2P-Regular.ttf"));

  const RobloxTextFontSelection selection =
      ResolveRobloxTextFont(13, assets.content().string());

  EXPECT_TRUE(selection.exact_mapping);
  EXPECT_EQ(selection.primary_file,
            (assets.content() / "fonts/PressStart2P-Regular.ttf").string());
  EXPECT_FLOAT_EQ(selection.size_scale, 0.9761678384F);
}

TEST(RobloxTextFontResolverTest, MatchesLegacyRbxKeyboardFallbacks) {
  TemporaryFontAssets assets;
  ASSERT_FALSE(assets.root().empty());
  ASSERT_TRUE(assets.CreateFile("assets/fonts/SourceSansPro-Bold.ttf"));

  const RobloxTextFontSelection selection =
      ResolveRobloxTextFont(4, assets.content().string());

  EXPECT_FALSE(selection.exact_mapping);
  EXPECT_EQ(selection.primary_file,
            (assets.root() / "assets/fonts/SourceSansPro-Bold.ttf").string());
  EXPECT_FLOAT_EQ(selection.size_scale, 0.795F);
}

TEST(RobloxTextFontResolverTest, RejectsMappingPathTraversal) {
  TemporaryFontAssets assets;
  ASSERT_FALSE(assets.root().empty());
  ASSERT_TRUE(assets.CreateFile(
      "assets/android/fonts/font-mappings.json",
      R"([{"enum":13,"font":"../../outside.ttf","fromRbxFontRatio":1.0}])"));
  ASSERT_TRUE(assets.CreateFile("assets/fonts/SourceSansPro-Regular.ttf"));

  const RobloxTextFontSelection selection =
      ResolveRobloxTextFont(13, assets.content().string());

  EXPECT_FALSE(selection.exact_mapping);
  EXPECT_EQ(
      selection.primary_file,
      (assets.root() / "assets/fonts/SourceSansPro-Regular.ttf").string());
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
