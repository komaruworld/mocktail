#include "runtime/host_os.h"

#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

namespace mocktail {
namespace runtime {
namespace {

bool StartsWith(std::string_view text, std::string_view prefix) {
  return text.size() >= prefix.size() &&
         text.compare(0, prefix.size(), prefix) == 0;
}

// True when token appears as a whitespace-separated entry in list (the
// ID_LIKE grammar).
bool ContainsToken(std::string_view list, std::string_view token) {
  std::size_t position = 0;
  while (position <= list.size()) {
    const std::size_t end = list.find_first_of(" \t", position);
    const std::string_view entry =
        list.substr(position, end == std::string_view::npos
                                   ? std::string_view::npos
                                   : end - position);
    if (entry == token) {
      return true;
    }
    if (end == std::string_view::npos) {
      break;
    }
    position = end + 1;
  }
  return false;
}

std::string_view Trim(std::string_view value) {
  const auto is_space = [](unsigned char character) {
    return character == ' ' || character == '\t';
  };
  while (!value.empty() && is_space(value.front())) {
    value.remove_prefix(1);
  }
  while (!value.empty() && is_space(value.back())) {
    value.remove_suffix(1);
  }
  return value;
}

// os-release values may be quoted with single or double quotes.
std::string Unquote(std::string_view value) {
  value = Trim(value);
  if (value.size() >= 2 &&
      ((value.front() == '"' && value.back() == '"') ||
       (value.front() == '\'' && value.back() == '\''))) {
    value = value.substr(1, value.size() - 2);
  }
  return std::string(value);
}

}  // namespace

bool HostOsInfo::is_bazzite() const {
  return id == "bazzite" || name == "Bazzite" ||
         StartsWith(variant_id, "bazzite") || StartsWith(image_id, "bazzite-");
}

bool HostOsInfo::is_fedora_family() const {
  return id == "fedora" || is_bazzite() || ContainsToken(id_like, "fedora");
}

bool HostOsInfo::is_immutable() const {
  return ostree_booted || !ostree_version.empty();
}

HostOsInfo ParseOsRelease(std::string_view content, bool ostree_booted) {
  HostOsInfo info;
  info.ostree_booted = ostree_booted;
  std::istringstream stream{std::string(content)};
  std::string line;
  while (std::getline(stream, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty() || line.front() == '#') {
      continue;
    }
    const std::size_t separator = line.find('=');
    if (separator == std::string::npos || separator == 0) {
      continue;
    }
    const std::string key = line.substr(0, separator);
    const std::string value = Unquote(line.substr(separator + 1));
    if (key == "ID") {
      info.id = value;
    } else if (key == "ID_LIKE") {
      info.id_like = value;
    } else if (key == "NAME") {
      info.name = value;
    } else if (key == "PRETTY_NAME") {
      info.pretty_name = value;
    } else if (key == "VERSION_ID") {
      info.version_id = value;
    } else if (key == "VARIANT") {
      info.variant = value;
    } else if (key == "VARIANT_ID") {
      info.variant_id = value;
    } else if (key == "IMAGE_ID") {
      info.image_id = value;
    } else if (key == "OSTREE_VERSION") {
      info.ostree_version = value;
    }
  }
  return info;
}

HostOsInfo DetectHostOsFromPaths(
    const std::vector<std::string>& os_release_paths,
    std::string_view ostree_marker_path) {
  bool ostree_booted = false;
  if (!ostree_marker_path.empty()) {
    std::error_code error;
    ostree_booted =
        std::filesystem::exists(std::filesystem::path(ostree_marker_path), error);
  }
  for (const std::string& path : os_release_paths) {
    std::ifstream file(path);
    if (!file) {
      continue;
    }
    std::ostringstream content;
    content << file.rdbuf();
    return ParseOsRelease(content.str(), ostree_booted);
  }
  HostOsInfo info;
  info.ostree_booted = ostree_booted;
  return info;
}

HostOsInfo DetectHostOs() {
  return DetectHostOsFromPaths(
      {"/run/host/os-release", "/etc/os-release", "/usr/lib/os-release"},
      "/run/ostree-booted");
}

std::string DescribeHostOs(const HostOsInfo& info) {
  std::string description =
      !info.pretty_name.empty() ? info.pretty_name
                                : (!info.name.empty() ? info.name : "unknown");
  if (!info.version_id.empty()) {
    description += ' ';
    description += info.version_id;
  }
  if (info.is_immutable()) {
    description += " (immutable)";
  }
  return description;
}

}  // namespace runtime
}  // namespace mocktail
