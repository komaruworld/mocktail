#include "libc_shim/vulkan_etc1_sky_transcoder.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include <gtest/gtest.h>

struct AAssetManager;
struct AAsset;

extern "C" {
AAssetManager* AAssetManager_fromJava(void* env, void* assetManager);
AAsset* AAssetManager_open(AAssetManager* mgr, const char* filename, int mode);
void AAsset_close(AAsset* asset);
int AAsset_read(AAsset* asset, void* buf, std::size_t count);
const void* AAsset_getBuffer(AAsset* asset);
off_t AAsset_getLength(AAsset* asset);
off_t AAsset_getRemainingLength(AAsset* asset);
off_t AAsset_seek(AAsset* asset, off_t offset, int whence);
int AAsset_openFileDescriptor(AAsset* asset, off_t* outStart,
                              off_t* outLength);
}

namespace {

class AssetManagerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    char path[] = "/tmp/mocktail-assets-XXXXXX";
    char* dir = mkdtemp(path);
    ASSERT_NE(dir, nullptr);
    root_ = dir;
    setenv("MOCKTAIL_ASSET_ROOT", root_.c_str(), 1);
  }

  void TearDown() override {
    unsetenv("MOCKTAIL_ASSET_ROOT");
    if (!root_.empty()) {
      std::filesystem::remove_all(root_);
    }
  }

  void WriteAsset(const std::string& name, const char* data) {
    std::string path = root_ + "/" + name;
    std::filesystem::create_directories(
        std::filesystem::path(path).parent_path());
    FILE* file = std::fopen(path.c_str(), "wb");
    ASSERT_NE(file, nullptr);
    ASSERT_EQ(std::fwrite(data, 1, std::strlen(data), file),
              std::strlen(data));
    ASSERT_EQ(std::fclose(file), 0);
  }

  std::string root_;
};

TEST_F(AssetManagerTest, FromJavaReturnsSingletonManager) {
  EXPECT_NE(AAssetManager_fromJava(nullptr, nullptr), nullptr);
}

TEST_F(AssetManagerTest, ConvertsSolidEtc1RgbBlockToOpaqueBc1) {
  const std::array<uint8_t, 8> etc1 = {0x55, 0x77, 0x99, 0, 0, 0, 0, 0};
  std::array<uint8_t, 8> bc1{};

  libc_shim::ConvertEtc1RgbBlockToBc1(etc1.data(), bc1.data());

  const uint16_t endpoint0 = static_cast<uint16_t>(bc1[0] | (bc1[1] << 8));
  const uint16_t endpoint1 = static_cast<uint16_t>(bc1[2] | (bc1[3] << 8));
  EXPECT_GT(endpoint0, endpoint1);
}

TEST_F(AssetManagerTest, OpenReadSeekAndBufferUseConfiguredAssetRoot) {
  WriteAsset("config.json", "mocktail-assets");

  AAssetManager* manager = AAssetManager_fromJava(nullptr, nullptr);
  AAsset* asset = AAssetManager_open(manager, "config.json", 0);
  ASSERT_NE(asset, nullptr);

  EXPECT_EQ(AAsset_getLength(asset), 15);
  char first[5] = {};
  EXPECT_EQ(AAsset_read(asset, first, 4), 4);
  EXPECT_STREQ(first, "mock");
  EXPECT_EQ(AAsset_getRemainingLength(asset), 11);

  EXPECT_EQ(AAsset_seek(asset, 9, SEEK_SET), 9);
  char second[7] = {};
  EXPECT_EQ(AAsset_read(asset, second, 6), 6);
  EXPECT_STREQ(second, "assets");

  const auto* buffer = static_cast<const char*>(AAsset_getBuffer(asset));
  ASSERT_NE(buffer, nullptr);
  EXPECT_EQ(std::string(buffer, buffer + AAsset_getLength(asset)),
            "mocktail-assets");

  AAsset_close(asset);
}

TEST_F(AssetManagerTest, OpensExtraContentModelsFromRbxAssetPaths) {
  WriteAsset("ExtraContent/models/UniversalApp/UniversalApp.rbxm", "rbxm");

  AAssetManager* manager = AAssetManager_fromJava(nullptr, nullptr);
  AAsset* stripped =
      AAssetManager_open(manager, "models/UniversalApp/UniversalApp.rbxm", 0);
  ASSERT_NE(stripped, nullptr);
  EXPECT_EQ(AAsset_getLength(stripped), 4);
  AAsset_close(stripped);

  AAsset* uri = AAssetManager_open(
      manager, "rbxasset://models/UniversalApp/UniversalApp.rbxm", 0);
  ASSERT_NE(uri, nullptr);
  EXPECT_EQ(AAsset_getLength(uri), 4);
  AAsset_close(uri);
}

TEST_F(AssetManagerTest, OpenFileDescriptorReturnsReadableFd) {
  WriteAsset("config.json", "fd-data");

  AAsset* asset =
      AAssetManager_open(AAssetManager_fromJava(nullptr, nullptr),
                         "assets/config.json", 0);
  ASSERT_NE(asset, nullptr);

  off_t start = -1;
  off_t length = -1;
  int fd = AAsset_openFileDescriptor(asset, &start, &length);
  ASSERT_GE(fd, 0);
  EXPECT_EQ(start, 0);
  EXPECT_EQ(length, 7);

  char buf[8] = {};
  EXPECT_EQ(read(fd, buf, sizeof(buf) - 1), 7);
  EXPECT_STREQ(buf, "fd-data");
  close(fd);
  AAsset_close(asset);
}

TEST_F(AssetManagerTest, MissingOrUnsafeAssetsReturnNull) {
  AAssetManager* manager = AAssetManager_fromJava(nullptr, nullptr);
  EXPECT_EQ(AAssetManager_open(manager, "missing.bin", 0), nullptr);
  EXPECT_EQ(AAssetManager_open(manager, "../config.json", 0), nullptr);
  EXPECT_EQ(AAssetManager_open(manager, "/tmp/config.json", 0), nullptr);
}

}  // namespace
