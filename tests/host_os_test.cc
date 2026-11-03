#include "runtime/host_os.h"

#include <gtest/gtest.h>

#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace mocktail {
namespace runtime {
namespace {

constexpr std::string_view kBazziteOsRelease = R"osrel(NAME="Bazzite"
VERSION="44.20260820.0 (Kinoite)"
RELEASE_TYPE=stable
ID=bazzite
ID_LIKE="fedora"
VERSION_ID=44
VERSION_CODENAME="Kinoite"
PRETTY_NAME="Bazzite"
CPE_NAME="cpe:/o:universal-blue:bazzite:44"
DEFAULT_HOSTNAME="bazzite"
VARIANT="Kinoite"
VARIANT_ID=bazzite-nvidia-open
OSTREE_VERSION='44.20260820.0'
IMAGE_ID="bazzite-nvidia-open-44.20260820"
)osrel";

constexpr std::string_view kFedoraWorkstationOsRelease = R"osrel(NAME="Fedora Linux"
VERSION="44 (Workstation Edition)"
ID=fedora
VERSION_ID=44
PRETTY_NAME="Fedora Linux 44 (Workstation Edition)"
ID_LIKE="fedora"
)osrel";

constexpr std::string_view kSilverblueOsRelease = R"osrel(NAME="Fedora Linux"
VERSION="44.20260820 (Silverblue)"
ID=fedora
VERSION_ID=44
PRETTY_NAME="Fedora Silverblue"
VARIANT="Silverblue"
VARIANT_ID=silverblue
OSTREE_VERSION='44.20260820.0'
)osrel";

TEST(HostOsParseTest, ParsesBazziteAtomicFields) {
  const HostOsInfo info = ParseOsRelease(kBazziteOsRelease, true);
  EXPECT_EQ(info.id, "bazzite");
  EXPECT_EQ(info.id_like, "fedora");
  EXPECT_EQ(info.name, "Bazzite");
  EXPECT_EQ(info.pretty_name, "Bazzite");
  EXPECT_EQ(info.version_id, "44");
  EXPECT_EQ(info.variant, "Kinoite");
  EXPECT_EQ(info.variant_id, "bazzite-nvidia-open");
  EXPECT_EQ(info.image_id, "bazzite-nvidia-open-44.20260820");
  EXPECT_EQ(info.ostree_version, "44.20260820.0");
  EXPECT_TRUE(info.ostree_booted);
  EXPECT_TRUE(info.is_bazzite());
  EXPECT_TRUE(info.is_fedora_family());
  EXPECT_TRUE(info.is_immutable());
}

TEST(HostOsParseTest, DetectsBazziteFromVariantIdWithoutMarker) {
  const HostOsInfo info = ParseOsRelease(kBazziteOsRelease, false);
  EXPECT_TRUE(info.is_bazzite());
  // OSTREE_VERSION alone still proves an image-based host.
  EXPECT_TRUE(info.is_immutable());
  EXPECT_FALSE(info.ostree_booted);
}

TEST(HostOsParseTest, FedoraWorkstationIsNotBazziteOrImmutable) {
  const HostOsInfo info = ParseOsRelease(kFedoraWorkstationOsRelease, false);
  EXPECT_EQ(info.id, "fedora");
  EXPECT_TRUE(info.is_fedora_family());
  EXPECT_FALSE(info.is_bazzite());
  EXPECT_FALSE(info.is_immutable());
}

TEST(HostOsParseTest, SilverblueIsImmutableFedoraFamily) {
  const HostOsInfo info = ParseOsRelease(kSilverblueOsRelease, true);
  EXPECT_TRUE(info.is_fedora_family());
  EXPECT_FALSE(info.is_bazzite());
  EXPECT_TRUE(info.is_immutable());
}

TEST(HostOsParseTest, EmptyContentYieldsUnknownDefaults) {
  const HostOsInfo info = ParseOsRelease("", false);
  EXPECT_TRUE(info.id.empty());
  EXPECT_FALSE(info.is_bazzite());
  EXPECT_FALSE(info.is_fedora_family());
  EXPECT_FALSE(info.is_immutable());
  EXPECT_EQ(DescribeHostOs(info), "unknown");
}

TEST(HostOsParseTest, IgnoresCommentsBlankLinesAndMalformedRows) {
  const HostOsInfo info = ParseOsRelease(
      "# comment\n\nID=fedora\nno-separator-line\n=orphan\nPRETTY_NAME=\"Fedora 44\"\r\n",
      false);
  EXPECT_EQ(info.id, "fedora");
  EXPECT_EQ(info.pretty_name, "Fedora 44");
  EXPECT_TRUE(info.is_fedora_family());
}

TEST(HostOsParseTest, IdLikeTokenMatchIsNotSubstring) {
  const HostOsInfo info = ParseOsRelease("ID=custom\nID_LIKE=\"myfedora thing\"\n", false);
  EXPECT_FALSE(info.is_fedora_family());
  const HostOsInfo spaced = ParseOsRelease("ID=custom\nID_LIKE=\"arch fedora opensuse\"\n", false);
  EXPECT_TRUE(spaced.is_fedora_family());
}

TEST(HostOsParseTest, DescribeHostOsFormatsPrettyNameAndVersion) {
  const HostOsInfo bazzite = ParseOsRelease(kBazziteOsRelease, true);
  EXPECT_EQ(DescribeHostOs(bazzite), "Bazzite 44 (immutable)");
  const HostOsInfo fedora = ParseOsRelease(kFedoraWorkstationOsRelease, false);
  EXPECT_EQ(DescribeHostOs(fedora), "Fedora Linux 44 (Workstation Edition) 44");
}

TEST(HostOsDetectTest, ReadsFirstAvailablePathAndMarker) {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() /
      ("mocktail-host-os-test-" + std::to_string(::getpid()));
  std::filesystem::create_directories(root);
  const std::filesystem::path host_release = root / "host-os-release";
  const std::filesystem::path runtime_release = root / "runtime-os-release";
  const std::filesystem::path marker = root / "ostree-booted";
  {
    std::ofstream(host_release) << kBazziteOsRelease;
    std::ofstream(runtime_release) << kFedoraWorkstationOsRelease;
  }

  const HostOsInfo host_first = DetectHostOsFromPaths(
      {host_release.string(), runtime_release.string()}, marker.string());
  EXPECT_TRUE(host_first.is_bazzite());
  EXPECT_FALSE(host_first.ostree_booted);
  EXPECT_TRUE(host_first.is_immutable());  // via OSTREE_VERSION

  std::ofstream(marker) << "";
  const HostOsInfo with_marker = DetectHostOsFromPaths(
      {runtime_release.string()}, marker.string());
  EXPECT_EQ(with_marker.id, "fedora");
  EXPECT_TRUE(with_marker.ostree_booted);
  EXPECT_TRUE(with_marker.is_immutable());

  const HostOsInfo missing = DetectHostOsFromPaths(
      {(root / "absent").string()}, (root / "absent-marker").string());
  EXPECT_TRUE(missing.id.empty());
  EXPECT_FALSE(missing.ostree_booted);
  EXPECT_FALSE(missing.is_immutable());

  std::filesystem::remove_all(root);
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
