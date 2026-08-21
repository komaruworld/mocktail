#include "runtime/roblox_desktop_app_policy.h"

#define JSON_NOEXCEPTION 1
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>

#include "runtime/runtime_paths.h"

namespace mocktail {
namespace runtime {
namespace {

constexpr std::uintmax_t kMaximumJsonBytes = 16U * 1024U * 1024U;
constexpr std::string_view kAppPolicySuffix = ":app-policy";
constexpr std::string_view kDesktopAppPolicyOverride =
    "FStringAppConfigurationOverrideAppPolicy";
std::atomic<std::uint64_t> g_temporary_sequence{0};

class ScopedFileDescriptor final {
 public:
  explicit ScopedFileDescriptor(int descriptor = -1)
      : descriptor_(descriptor) {}
  ~ScopedFileDescriptor() {
    if (descriptor_ >= 0) {
      close(descriptor_);
    }
  }

  ScopedFileDescriptor(const ScopedFileDescriptor&) = delete;
  ScopedFileDescriptor& operator=(const ScopedFileDescriptor&) = delete;

  int get() const { return descriptor_; }

 private:
  int descriptor_ = -1;
};

enum class ReadStatus { kMissing, kReady, kInvalid };

struct JsonReadResult {
  ReadStatus status = ReadStatus::kMissing;
  nlohmann::json value;
  std::string error;
};

bool WriteAll(int descriptor, std::string_view bytes) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t written =
        write(descriptor, bytes.data() + offset, bytes.size() - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return false;
    }
    if (written == 0) {
      return false;
    }
    offset += static_cast<std::size_t>(written);
  }
  return true;
}

JsonReadResult ReadJsonObject(const std::filesystem::path& path,
                              std::string_view label) {
  const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    if (errno == ENOENT) {
      return {};
    }
    return {
        ReadStatus::kInvalid, {}, "cannot safely open " + std::string(label)};
  }
  const ScopedFileDescriptor file(descriptor);
  struct stat status{};
  if (fstat(file.get(), &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_size < 0 ||
      static_cast<std::uintmax_t>(status.st_size) > kMaximumJsonBytes) {
    return {ReadStatus::kInvalid,
            {},
            std::string(label) + " is not a bounded regular file"};
  }

  std::string bytes(static_cast<std::size_t>(status.st_size), '\0');
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t count =
        read(file.get(), bytes.data() + offset, bytes.size() - offset);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      return {ReadStatus::kInvalid, {}, "cannot read " + std::string(label)};
    }
    if (count == 0) {
      return {ReadStatus::kInvalid,
              {},
              std::string(label) + " changed while being read"};
    }
    offset += static_cast<std::size_t>(count);
  }
  nlohmann::json parsed = nlohmann::json::parse(bytes, nullptr, false, true);
  if (parsed.is_discarded() || !parsed.is_object()) {
    return {
        ReadStatus::kInvalid, {}, std::string(label) + " is not a JSON object"};
  }
  return {ReadStatus::kReady, std::move(parsed), {}};
}

bool AtomicWriteJson(const std::filesystem::path& path,
                     const nlohmann::json& value, std::string* error) {
  std::error_code filesystem_error;
  if (!RuntimePaths::EnsureDirectory(path.parent_path(), &filesystem_error)) {
    *error = "cannot create desktop app-policy directory";
    return false;
  }
  const std::filesystem::file_status target_status =
      std::filesystem::symlink_status(path, filesystem_error);
  if (!filesystem_error && std::filesystem::is_symlink(target_status)) {
    *error = "desktop app-policy target is a symlink";
    return false;
  }
  if (filesystem_error != std::errc::no_such_file_or_directory &&
      filesystem_error) {
    *error = "cannot inspect desktop app-policy target";
    return false;
  }

  const std::filesystem::path temporary =
      path.parent_path() /
      (".mocktail-desktop-policy.tmp." + std::to_string(getpid()) + "." +
       std::to_string(g_temporary_sequence.fetch_add(1)));
  const int descriptor =
      open(temporary.c_str(),
           O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (descriptor < 0) {
    *error = "cannot create temporary desktop app-policy";
    return false;
  }
  bool stored = false;
  {
    const ScopedFileDescriptor file(descriptor);
    const std::string bytes = value.dump();
    stored = fchmod(file.get(), S_IRUSR | S_IWUSR) == 0 &&
             WriteAll(file.get(), bytes) && fsync(file.get()) == 0;
  }
  if (!stored || rename(temporary.c_str(), path.c_str()) != 0) {
    const int saved_errno = errno;
    (void)unlink(temporary.c_str());
    errno = saved_errno;
    *error = "cannot atomically store desktop app-policy";
    return false;
  }
  const int directory_descriptor =
      open(path.parent_path().c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
  if (directory_descriptor < 0) {
    *error = "cannot open desktop app-policy directory for sync";
    return false;
  }
  const ScopedFileDescriptor directory(directory_descriptor);
  if (fsync(directory.get()) != 0) {
    *error = "cannot sync desktop app-policy directory";
    return false;
  }
  return true;
}

bool HasSuffix(std::string_view value, std::string_view suffix) {
  return value.size() >= suffix.size() &&
         value.compare(value.size() - suffix.size(), suffix.size(), suffix) ==
             0;
}

void NormalizeDesktopLayout(nlohmann::json* policy) {
  (*policy)["PlatformGroup"] = "Desktop";
  (*policy)["UseGridHomePage"] = true;
  (*policy)["UseGridPageLayout"] = true;
  (*policy)["SystemBarPlacement"] = "Left";
  (*policy)["ShouldSystemBarUsuallyBePresent"] = true;
  (*policy)["DevicePreferencesPersistentPresenceVariant"] = "windows";
}

bool DecodePolicy(const nlohmann::json& encoded, nlohmann::json* policy) {
  if (!encoded.is_string()) {
    return false;
  }
  *policy = nlohmann::json::parse(encoded.get_ref<const std::string&>(),
                                  nullptr, false, true);
  return !policy->is_discarded() && policy->is_object();
}

}  // namespace

DesktopAppPolicyResult ApplyDesktopAppPolicy(
    const std::filesystem::path& app_storage_file,
    const std::filesystem::path& default_policy_file,
    std::int64_t authenticated_user_id) {
  DesktopAppPolicyResult result;
  if (!app_storage_file.is_absolute() || !default_policy_file.is_absolute() ||
      authenticated_user_id < -1) {
    result.error = "desktop app-policy paths or user ID are invalid";
    return result;
  }

  JsonReadResult storage =
      ReadJsonObject(app_storage_file, "Roblox appStorage");
  if (storage.status == ReadStatus::kInvalid) {
    result.error = storage.error;
    return result;
  }
  if (storage.status == ReadStatus::kMissing) {
    storage.status = ReadStatus::kReady;
    storage.value = nlohmann::json::object();
    result.app_storage_created = true;
  }

  nlohmann::json configurations = nlohmann::json::object();
  const auto encoded_configurations = storage.value.find("AppConfiguration");
  if (encoded_configurations != storage.value.end()) {
    if (!encoded_configurations->is_string()) {
      result.error = "Roblox AppConfiguration is not an encoded object";
      return result;
    }
    configurations = nlohmann::json::parse(
        encoded_configurations->get_ref<const std::string&>(), nullptr, false,
        true);
    if (configurations.is_discarded() || !configurations.is_object()) {
      result.error = "Roblox AppConfiguration encoding is invalid";
      return result;
    }
  }

  nlohmann::json template_policy;
  nlohmann::json guest_policy;
  nlohmann::json authenticated_policy;
  const std::string authenticated_key =
      authenticated_user_id >= 0
          ? "GUAC:" + std::to_string(authenticated_user_id) + ":app-policy"
          : std::string();
  for (auto& entry : configurations.items()) {
    if (!HasSuffix(entry.key(), kAppPolicySuffix)) {
      continue;
    }
    nlohmann::json policy;
    if (!DecodePolicy(entry.value(), &policy)) {
      result.error = "cached Roblox app-policy encoding is invalid";
      return result;
    }
    NormalizeDesktopLayout(&policy);
    entry.value() = policy.dump();
    if (!template_policy.is_object()) {
      template_policy = policy;
    }
    if (entry.key() == "GUAC:-1:app-policy") {
      guest_policy = policy;
    }
    if (!authenticated_key.empty() && entry.key() == authenticated_key) {
      authenticated_policy = policy;
    }
    ++result.normalized_policy_count;
  }

  if (authenticated_policy.is_object()) {
    template_policy = std::move(authenticated_policy);
  } else if (guest_policy.is_object()) {
    template_policy = std::move(guest_policy);
  }

  if (!template_policy.is_object()) {
    const JsonReadResult fallback =
        ReadJsonObject(default_policy_file, "Roblox default app-policy");
    if (fallback.status != ReadStatus::kReady) {
      result.error = fallback.status == ReadStatus::kMissing
                         ? "Roblox default app-policy is missing"
                         : fallback.error;
      return result;
    }
    template_policy = fallback.value;
    NormalizeDesktopLayout(&template_policy);
  }

  const std::string encoded_policy = template_policy.dump();
  result.policy_json = encoded_policy;
  configurations["GUAC:-1:app-policy"] = encoded_policy;
  if (authenticated_user_id >= 0) {
    configurations[authenticated_key] = encoded_policy;
  }

  const std::string encoded = configurations.dump();
  const bool changed =
      encoded_configurations == storage.value.end() ||
      encoded_configurations->get_ref<const std::string&>() != encoded;
  if (!changed) {
    return result;
  }
  storage.value["AppConfiguration"] = encoded;
  if (!AtomicWriteJson(app_storage_file, storage.value, &result.error)) {
    return result;
  }
  result.updated = true;
  return result;
}

bool MergeDesktopAppPolicyClientSettingsOverride(std::string_view policy_json,
                                                 std::string_view base_json,
                                                 std::string* merged_json,
                                                 std::string* error) {
  if (merged_json == nullptr) {
    if (error != nullptr) {
      *error = "desktop app-policy client-settings output is required";
    }
    return false;
  }
  const nlohmann::json policy =
      nlohmann::json::parse(policy_json, nullptr, false, true);
  if (policy.is_discarded() || !policy.is_object()) {
    if (error != nullptr) {
      *error = "desktop app-policy override must be a JSON object";
    }
    return false;
  }
  nlohmann::json overrides = nlohmann::json::parse(
      base_json.empty() ? "{}" : base_json, nullptr, false, true);
  if (overrides.is_discarded() || !overrides.is_object()) {
    if (error != nullptr) {
      *error = "client-settings overrides must be a JSON object";
    }
    return false;
  }
  const auto existing = overrides.find(kDesktopAppPolicyOverride);
  if (existing != overrides.end() &&
      (!existing->is_string() ||
       existing->get_ref<const std::string&>() != policy_json)) {
    if (error != nullptr) {
      *error = "desktop app-policy conflicts with " +
               std::string(kDesktopAppPolicyOverride);
    }
    return false;
  }
  overrides[std::string(kDesktopAppPolicyOverride)] = policy_json;
  *merged_json = overrides.dump();
  return true;
}

}  // namespace runtime
}  // namespace mocktail
