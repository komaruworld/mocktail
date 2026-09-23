#include "runtime/temporary_instance.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "runtime/runtime_config_file.h"

namespace mocktail {
namespace runtime {
namespace {

constexpr std::string_view kInstancesDirectory = "instances";
constexpr std::string_view kTemporaryPrefix = "temp-";

bool Enabled(const Environment& environment, std::string_view name) {
  const std::optional<std::string> value = environment.Get(name);
  return value.has_value() && !value->empty() && *value != "0";
}

bool SetEnvironment(const char* name, const std::string& value,
                    std::string* error) {
  if (setenv(name, value.c_str(), 1) == 0) {
    return true;
  }
  if (error != nullptr) {
    *error = std::string("cannot export temporary instance path: ") + name;
  }
  return false;
}

bool SetEnvironment(const char* name, const std::filesystem::path& value,
                    std::string* error) {
  return SetEnvironment(name, value.string(), error);
}

bool CreatePrivateDirectory(const std::filesystem::path& path,
                            std::string* error) {
  if (mkdir(path.c_str(), 0700) != 0 && errno != EEXIST) {
    if (error != nullptr) {
      *error = "cannot create temporary instance directory " +
               path.string() + ": " + std::strerror(errno);
    }
    return false;
  }
  return true;
}

std::filesystem::path InstancesRoot(const RuntimePaths& primary_paths) {
  return primary_paths.state_root() / std::string(kInstancesDirectory);
}

bool IsManagedTemporaryRoot(const std::filesystem::path& path) {
  return path.filename().string().rfind(kTemporaryPrefix, 0) == 0 &&
         path.parent_path().filename() == kInstancesDirectory;
}

bool TemporaryRootSettling(const std::filesystem::path& root) {
  std::error_code error;
  const auto modified = std::filesystem::last_write_time(root, error);
  if (error) {
    return false;
  }
  return std::filesystem::file_time_type::clock::now() - modified <
         std::chrono::seconds(30);
}

bool TemporaryRootOwnedByLiveInstance(const std::filesystem::path& root) {
  const std::filesystem::path lock_file = root / "state/instance.lock";
  const int descriptor =
      open(lock_file.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    return false;
  }
  int lock_result = -1;
  do {
    lock_result = flock(descriptor, LOCK_EX | LOCK_NB);
  } while (lock_result != 0 && errno == EINTR);
  const bool owned = lock_result != 0 &&
                     (errno == EWOULDBLOCK || errno == EAGAIN);
  close(descriptor);
  return owned;
}

void SweepStaleTemporaryInstances(const RuntimePaths& primary_paths) {
  const std::filesystem::path instances = InstancesRoot(primary_paths);
  std::error_code error;
  std::filesystem::directory_iterator iterator(instances, error);
  const std::filesystem::directory_iterator end;
  while (!error && iterator != end) {
    const std::filesystem::path entry = iterator->path();
    const std::filesystem::file_status status =
        std::filesystem::symlink_status(entry, error);
    if (!error && std::filesystem::is_directory(status) &&
        !std::filesystem::is_symlink(status) &&
        IsManagedTemporaryRoot(entry) &&
        !TemporaryRootSettling(entry) &&
        !TemporaryRootOwnedByLiveInstance(entry)) {
      std::error_code remove_error;
      std::filesystem::remove_all(entry, remove_error);
    }
    error.clear();
    iterator.increment(error);
  }
}

}  // namespace

TemporaryInstanceResult PrepareTemporaryInstance(
    const Environment& environment, const RuntimePaths& primary_paths) {
  TemporaryInstanceResult result;
  if (Enabled(environment, "MOCKTAIL_ISOLATED_CANARY")) {
    result.error =
        "temporary instances cannot run inside an isolated canary";
    return result;
  }
  const std::optional<std::string> existing_root =
      environment.Get("MOCKTAIL_TEMP_INSTANCE_ROOT");
  if (existing_root.has_value() && !existing_root->empty()) {
    std::string error;
    if (!SetEnvironment("MOCKTAIL_TEMP_INSTANCE", std::string("1"), &error)) {
      result.error = std::move(error);
      return result;
    }
    result.root = *existing_root;
    return result;
  }

  const bool has_library = environment.HasNonEmpty("ROBLOX_LIB_PATH");
  const bool has_assets = environment.HasNonEmpty("MOCKTAIL_ASSET_PATH");
  ActivePayloadPaths active;
  if (!has_library) {
    active = primary_paths.ResolveActivePayload();
    if (!active || !active.active) {
      result.error =
          "temporary instance requires an activated managed payload; "
          "launch Mocktail normally first" +
          (active.error.empty() ? std::string() : ": " + active.error);
      return result;
    }
  }
  std::filesystem::path assets_path = active.assets_content;
  if (has_library && !has_assets) {
    assets_path = ResolveAdjacentRobloxAssetPath(
        environment.GetOr("ROBLOX_LIB_PATH", ""),
        primary_paths.working_directory());
    if (assets_path.empty()) {
      result.error = "cannot bind assets to explicit Roblox library";
      return result;
    }
  }

  SweepStaleTemporaryInstances(primary_paths);
  const std::filesystem::path instances = InstancesRoot(primary_paths);
  std::error_code parent_error;
  std::filesystem::create_directories(instances.parent_path(), parent_error);
  if (parent_error) {
    result.error = "cannot create state root " +
                   instances.parent_path().string() + ": " +
                   parent_error.message();
    return result;
  }
  std::string error;
  if (!CreatePrivateDirectory(instances, &error)) {
    result.error = std::move(error);
    return result;
  }
  const std::string pattern = (instances / "temp-XXXXXX").string();
  std::vector<char> writable(pattern.begin(), pattern.end());
  writable.push_back('\0');
  const char* created = mkdtemp(writable.data());
  if (created == nullptr) {
    result.error = "cannot create temporary instance root in " +
                   instances.string() + ": " + std::strerror(errno);
    return result;
  }
  const std::filesystem::path root = created;
  const auto fail = [&result, &root](std::string message) {
    std::error_code cleanup_error;
    std::filesystem::remove_all(root, cleanup_error);
    result.error = std::move(message);
    return result;
  };
  if (!CreatePrivateDirectory(root / "data", &error) ||
      !CreatePrivateDirectory(root / "cache", &error) ||
      !CreatePrivateDirectory(root / "state", &error)) {
    return fail(std::move(error));
  }
  if (!SetEnvironment("MOCKTAIL_DATA_ROOT", root / "data", &error) ||
      !SetEnvironment("MOCKTAIL_CACHE_ROOT", root / "cache", &error) ||
      !SetEnvironment("MOCKTAIL_STATE_ROOT", root / "state", &error) ||
      !SetEnvironment("MOCKTAIL_AUTH_ROOT", root / "data/auth", &error) ||
      (!has_library &&
       !SetEnvironment("ROBLOX_LIB_PATH", active.roblox_library, &error)) ||
      (!has_assets &&
       !SetEnvironment("MOCKTAIL_ASSET_PATH", assets_path, &error)) ||
      !SetEnvironment("MOCKTAIL_TEMP_INSTANCE", std::string("1"), &error) ||
      !SetEnvironment("MOCKTAIL_TEMP_INSTANCE_ROOT", root, &error) ||
      !SetEnvironment("MOCKTAIL_SKIP_UPDATE_CHECK", std::string("1"),
                      &error)) {
    return fail(std::move(error));
  }
  if (!active.compatibility_manifest.empty() &&
      (!SetEnvironment("MOCKTAIL_COMPATIBILITY_MANIFEST",
                       active.compatibility_manifest, &error) ||
       !SetEnvironment("MOCKTAIL_HOST_ABI_PROFILE_FILE",
                       active.host_abi_profile, &error) ||
       !SetEnvironment("MOCKTAIL_HOST_ABI_APPROVAL_RECEIPT",
                       active.host_abi_approval_receipt, &error))) {
    return fail(std::move(error));
  }
  result.root = root;
  return result;
}

TemporaryInstance::TemporaryInstance(std::filesystem::path root)
    : root_(std::move(root)) {}

TemporaryInstance::~TemporaryInstance() { Remove(); }

TemporaryInstance::TemporaryInstance(TemporaryInstance&& other) noexcept
    : root_(std::move(other.root_)) {
  other.root_.clear();
}

TemporaryInstance& TemporaryInstance::operator=(
    TemporaryInstance&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  Remove();
  root_ = std::move(other.root_);
  other.root_.clear();
  return *this;
}

void TemporaryInstance::Remove() {
  if (root_.empty()) {
    return;
  }
  const std::filesystem::path root = std::move(root_);
  root_.clear();
  if (!IsManagedTemporaryRoot(root)) {
    return;
  }
  std::error_code error;
  std::filesystem::remove_all(root, error);
}

bool TemporaryInstanceWindowsEnabled(
    const Environment& environment,
    const std::filesystem::path& config_file) {
  const RuntimeConfigLoadResult loaded =
      LoadRuntimeConfig(environment, config_file);
  return loaded && loaded.config.temporary_instance_windows();
}

}  // namespace runtime
}  // namespace mocktail
