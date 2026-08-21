#include "runtime/support_bundle.h"

#define JSON_NOEXCEPTION 1
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mocktail::runtime {
namespace {

bool Enabled(const Environment& environment, std::string_view name) {
  const std::optional<std::string> value = environment.Get(name);
  return value.has_value() && !value->empty() && *value != "0";
}

bool IsDiagnosticToken(std::string_view value) {
  if (value.empty() || value.size() > 64 || !std::isalnum(value.front())) {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](unsigned char character) {
    return std::isalnum(character) || character == '.' || character == '_' ||
           character == ':' || character == '+' || character == '-';
  });
}

bool WritePrivate(const std::filesystem::path& path, std::string_view contents,
                  std::string* error) {
  const int descriptor = open(
      path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (descriptor < 0) {
    *error = "cannot create native support-bundle file";
    return false;
  }
  std::size_t offset = 0;
  while (offset < contents.size()) {
    const ssize_t bytes =
        write(descriptor, contents.data() + offset, contents.size() - offset);
    if (bytes < 0) {
      if (errno == EINTR) continue;
      close(descriptor);
      *error = "cannot write native support-bundle file";
      return false;
    }
    offset += static_cast<std::size_t>(bytes);
  }
  const bool synced = fsync(descriptor) == 0;
  close(descriptor);
  if (!synced) *error = "cannot persist native support-bundle file";
  return synced;
}

std::string ReadTail(const std::filesystem::path& path, std::size_t maximum) {
  if (path.empty()) return {};
  const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) return {};
  struct stat metadata = {};
  if (fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
      metadata.st_size < 0) {
    close(descriptor);
    return {};
  }
  const off_t start = metadata.st_size > static_cast<off_t>(maximum)
                          ? metadata.st_size - static_cast<off_t>(maximum)
                          : 0;
  if (lseek(descriptor, start, SEEK_SET) < 0) {
    close(descriptor);
    return {};
  }
  std::string contents;
  std::array<char, 16U * 1024U> buffer{};
  while (contents.size() < maximum) {
    const std::size_t requested =
        std::min(buffer.size(), maximum - contents.size());
    const ssize_t bytes = read(descriptor, buffer.data(), requested);
    if (bytes == 0) break;
    if (bytes < 0) {
      if (errno == EINTR) continue;
      contents.clear();
      break;
    }
    contents.append(buffer.data(), static_cast<std::size_t>(bytes));
  }
  close(descriptor);
  return contents;
}

bool SafeLogLine(std::string_view line) {
  constexpr std::array<std::string_view, 15> kAllowlistedMarkers = {
      "[FATAL]",
      "[compat]",
      "[window] first Roblox Vulkan frame presented",
      "[vulkan]",
      "[mocktail][audio]",
      "[input]",
      "[lifecycle]",
      "[main] Roblox lifecycle shutdown: Stopped",
      "setStage:",
      "JNI_OnLoad",
      "RenderView created",
      "RenderView destroyed",
      "[native-updater]",
      "Segmentation fault",
      "core dumped",
  };
  if (line.find(".ROBLOSECURITY") != line.npos ||
      line.find("Authorization:") != line.npos ||
      line.find("authenticationTicket") != line.npos ||
      line.find("http://") != line.npos || line.find("https://") != line.npos) {
    return false;
  }
  return std::any_of(
      kAllowlistedMarkers.begin(), kAllowlistedMarkers.end(),
      [&](std::string_view marker) { return line.find(marker) != line.npos; });
}

std::string SanitizeLog(std::string_view raw) {
  std::string output;
  for (std::size_t begin = 0; begin < raw.size();) {
    std::size_t end = raw.find('\n', begin);
    if (end == raw.npos) end = raw.size();
    std::string_view line = raw.substr(begin, end - begin);
    if (SafeLogLine(line)) {
      // Bound individual lines so future guest text cannot inflate a bundle.
      if (line.size() > 1024) line = line.substr(0, 1024);
      output.append(line);
      output.push_back('\n');
    }
    begin = end + (end < raw.size() ? 1U : 0U);
  }
  return output;
}

std::string PayloadSummary(const RuntimePaths& paths) {
  const std::string manifest =
      ReadTail(paths.active_payload_manifest(), 1024U * 1024U);
  const nlohmann::json document =
      nlohmann::json::parse(manifest, nullptr, false, true);
  if (document.is_discarded() || !document.is_object()) {
    return "status=unavailable\n";
  }
  const std::string payload_id = document.value("payload_id", "");
  const std::string version_name = document.value("version_name", "");
  const std::string build_id = document.value("elf_build_id", "");
  if (!IsDiagnosticToken(payload_id) || !IsDiagnosticToken(version_name) ||
      build_id.size() != 40) {
    return "status=unavailable\n";
  }
  return "status=active\npayload_id=" + payload_id +
         "\nversion_name=" + version_name + "\nelf_build_id=" + build_id + "\n";
}

std::size_t MaximumLogBytes(const Environment& environment) {
  const auto value = environment.Get("MOCKTAIL_SUPPORT_MAX_LOG_BYTES");
  if (!value.has_value()) return 256U * 1024U;
  std::size_t parsed = 0;
  for (const unsigned char character : *value) {
    if (!std::isdigit(character) || parsed > 1048576U) return 256U * 1024U;
    parsed = parsed * 10U + static_cast<std::size_t>(character - '0');
  }
  return parsed >= 1000U && parsed <= 1048576U ? parsed : 256U * 1024U;
}

void PruneBundles(const std::filesystem::path& root) {
  std::error_code error;
  std::vector<std::filesystem::directory_entry> bundles;
  for (std::filesystem::directory_iterator iterator(root, error), end;
       !error && iterator != end; iterator.increment(error)) {
    if (iterator->is_directory(error) &&
        iterator->path().filename().string().find("mocktail-support-") == 0) {
      bundles.push_back(*iterator);
    }
  }
  if (error || bundles.size() <= 5) return;
  std::sort(bundles.begin(), bundles.end(),
            [&](const auto& left, const auto& right) {
              return left.last_write_time(error) > right.last_write_time(error);
            });
  for (std::size_t index = 5; index < bundles.size(); ++index) {
    std::filesystem::remove_all(bundles[index].path(), error);
    error.clear();
  }
}

}  // namespace

SupportBundleCollectionResult CollectFailureSupportBundle(
    const Environment& environment, const RuntimePaths& paths,
    std::string_view context, std::string_view reason, int exit_code,
    const std::filesystem::path& source_log) {
  SupportBundleCollectionResult result;
  if (Enabled(environment, "MOCKTAIL_DISABLE_SUPPORT_BUNDLE")) return result;
  if (!IsDiagnosticToken(context) || !IsDiagnosticToken(reason) ||
      exit_code < 0 || exit_code > 255) {
    result.error = "invalid support bundle request";
    return result;
  }
  result.attempted = true;
  const std::filesystem::path output_root =
      environment.HasNonEmpty("MOCKTAIL_SUPPORT_OUTPUT_ROOT")
          ? std::filesystem::path(
                environment.GetOr("MOCKTAIL_SUPPORT_OUTPUT_ROOT", ""))
          : paths.state_root() / "support";
  if (!output_root.is_absolute() || output_root == paths.state_root() ||
      output_root == paths.data_root() || output_root == paths.cache_root()) {
    result.error = "support output must be a dedicated absolute directory";
    return result;
  }
  std::error_code error;
  std::filesystem::create_directories(output_root, error);
  std::filesystem::permissions(output_root, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace, error);
  if (error) {
    result.error = "cannot create private support directory";
    return result;
  }
  const auto epoch = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
  const std::filesystem::path bundle =
      output_root / ("mocktail-support-" + std::to_string(epoch) + "-" +
                     std::to_string(getpid()));
  if (!std::filesystem::create_directory(bundle, error)) {
    result.error = "cannot create native support bundle";
    return result;
  }
  std::filesystem::permissions(bundle, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace, error);
  struct utsname system = {};
  (void)uname(&system);
  const std::string runtime =
      "schema=2\nformat=native-directory\ncontext=" + std::string(context) +
      "\nreason=" + std::string(reason) +
      "\nexit_code=" + std::to_string(exit_code) +
      "\nkernel_name=" + system.sysname + "\nkernel_release=" + system.release +
      "\narchitecture=" + system.machine + "\n";
  const std::string recent =
      SanitizeLog(ReadTail(source_log, MaximumLogBytes(environment)));
  if (!WritePrivate(bundle / "runtime.txt", runtime, &result.error) ||
      !WritePrivate(bundle / "payload.txt", PayloadSummary(paths),
                    &result.error) ||
      !WritePrivate(bundle / "recent.log", recent, &result.error)) {
    std::filesystem::remove_all(bundle, error);
    return result;
  }
  PruneBundles(output_root);
  std::cerr << "  [support] native bundle: " << bundle << '\n';
  return result;
}

FailureSupportBundleGuard::FailureSupportBundleGuard(
    const Environment& environment, const RuntimePaths& paths, bool armed)
    : environment_(&environment), paths_(&paths), armed_(armed) {}

FailureSupportBundleGuard::~FailureSupportBundleGuard() {
  if (!armed_ || environment_ == nullptr || paths_ == nullptr) return;
  const SupportBundleCollectionResult result = CollectFailureSupportBundle(
      *environment_, *paths_, "launch", "launch-failed", exit_code_);
  if (!result) std::cerr << "  [support] " << result.error << '\n';
}

void FailureSupportBundleGuard::SetExitCode(int exit_code) {
  if (exit_code >= 0 && exit_code <= 255) exit_code_ = exit_code;
}

void FailureSupportBundleGuard::Disarm() { armed_ = false; }

}  // namespace mocktail::runtime
