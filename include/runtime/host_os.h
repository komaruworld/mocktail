#ifndef MOCKTAIL_RUNTIME_HOST_OS_H_
#define MOCKTAIL_RUNTIME_HOST_OS_H_

#include <string>
#include <string_view>
#include <vector>

namespace mocktail {
namespace runtime {

// Parsed view of os-release(5). Immutable (rpm-ostree/bootc) hosts such as
// Bazzite carry OSTREE_VERSION, VARIANT_ID=bazzite-*, and a /run/ostree-booted
// marker; inside a Flatpak sandbox the host file is exposed at
// /run/host/os-release, which DetectHostOs() prefers over the runtime copy.
struct HostOsInfo {
  std::string id;
  std::string id_like;
  std::string name;
  std::string pretty_name;
  std::string version_id;
  std::string variant;
  std::string variant_id;
  std::string image_id;
  std::string ostree_version;
  bool ostree_booted = false;

  // True for Bazzite and its image variants (ID=bazzite, VARIANT_ID=bazzite-*,
  // IMAGE_ID=bazzite-*, or NAME="Bazzite").
  bool is_bazzite() const;
  // True for Fedora itself and every ID_LIKE=fedora derivative (Bazzite
  // included).
  bool is_fedora_family() const;
  // True on image-based hosts: the ostree-booted marker exists or the
  // deployment reports an OSTREE_VERSION.
  bool is_immutable() const;
};

// Parses os-release content. Pure and deterministic; tests feed fixtures.
HostOsInfo ParseOsRelease(std::string_view content, bool ostree_booted);

// Reads the first readable path in os_release_paths and checks the ostree
// marker. Returns a default-constructed HostOsInfo (with the marker flag)
// when no os-release file exists.
HostOsInfo DetectHostOsFromPaths(const std::vector<std::string>& os_release_paths,
                                 std::string_view ostree_marker_path);

// Production detection: /run/host/os-release (host view inside Flatpak),
// then /etc/os-release, then /usr/lib/os-release, plus /run/ostree-booted.
HostOsInfo DetectHostOs();

// Short human-readable summary, e.g. "Bazzite 44 (immutable)".
std::string DescribeHostOs(const HostOsInfo& info);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_HOST_OS_H_
