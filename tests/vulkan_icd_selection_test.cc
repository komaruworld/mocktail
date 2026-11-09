#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <vector>

#include "runtime/graphics_launch_policy.h"

#include "compat/guest_abi.h"

namespace mocktail {
namespace runtime {
namespace {

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    char pattern[] = "/tmp/mocktail_icd_XXXXXX";
    const char* created = mkdtemp(pattern);
    if (created != nullptr) root_ = created;
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  const std::filesystem::path& root() const { return root_; }

 private:
  std::filesystem::path root_;
};

void WriteManifest(const std::filesystem::path& path) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream(path) << R"({"ICD":{"library_path":"libGLX_nvidia.so.0"}})";
}

TEST(VulkanIcdSelectionTest, PrefersTheNativeArchitectureManifest) {
  TemporaryDirectory temporary;
  WriteManifest(temporary.root() / "nvidia_icd.i686.json");
  const std::string native_name =
      "nvidia_icd." + std::string(compat::kGuestCpuName) + ".json";
  WriteManifest(temporary.root() / native_name);

  EXPECT_EQ(SelectVulkanIcdManifest({temporary.root()}, "nvidia_icd"),
            (temporary.root() / native_name).string());
}

TEST(VulkanIcdSelectionTest, NeverSelectsAForeignArchitectureManifest) {
  TemporaryDirectory temporary;
  WriteManifest(temporary.root() / "nvidia_icd.i686.json");
#if defined(__aarch64__)
  WriteManifest(temporary.root() / "radeon_icd.x86_64.json");
#else
  WriteManifest(temporary.root() / "radeon_icd.aarch64.json");
#endif

  EXPECT_TRUE(
      SelectVulkanIcdManifest({temporary.root()}, "nvidia_icd").empty());
  EXPECT_TRUE(
      SelectVulkanIcdManifest({temporary.root()}, "radeon_icd").empty());
}

TEST(VulkanIcdSelectionTest, AcceptsAnUnqualifiedManifest) {
  TemporaryDirectory temporary;
  WriteManifest(temporary.root() / "nvidia_icd.json");

  EXPECT_EQ(SelectVulkanIcdManifest({temporary.root()}, "nvidia_icd"),
            (temporary.root() / "nvidia_icd.json").string());
}

TEST(VulkanIcdSelectionTest, KeepsDirectoryPrecedenceOverName) {
  TemporaryDirectory temporary;
  const std::filesystem::path first = temporary.root() / "first";
  const std::filesystem::path second = temporary.root() / "second";
  WriteManifest(first / "nvidia_icd.json");
  WriteManifest(second / ("nvidia_icd." +
                          std::string(compat::kGuestCpuName) + ".json"));

  EXPECT_EQ(SelectVulkanIcdManifest({first, second}, "nvidia_icd"),
            (first / "nvidia_icd.json").string());
}

TEST(VulkanIcdSelectionTest, IsStableWhateverTheDirectoryOrderIs) {
  TemporaryDirectory temporary;
  const std::string native_name =
      "nvidia_icd." + std::string(compat::kGuestCpuName) + ".json";
  WriteManifest(temporary.root() / native_name);
  WriteManifest(temporary.root() / "nvidia_icd.json");
  WriteManifest(temporary.root() / "nvidia_icd.i686.json");

  const std::string selected =
      SelectVulkanIcdManifest({temporary.root()}, "nvidia_icd");
  EXPECT_EQ(selected, (temporary.root() / native_name).string());
  EXPECT_EQ(selected, SelectVulkanIcdManifest({temporary.root()},
                                              "nvidia_icd"));
}

TEST(VulkanIcdSelectionTest, IgnoresAMissingDirectory) {
  TemporaryDirectory temporary;
  EXPECT_TRUE(SelectVulkanIcdManifest({temporary.root() / "absent"},
                                      "nvidia_icd")
                  .empty());
  EXPECT_TRUE(SelectVulkanIcdManifest({temporary.root()}, "").empty());
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
