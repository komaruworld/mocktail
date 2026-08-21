#include "runtime/single_instance_lock.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace mocktail {
namespace runtime {
namespace {

std::string ExecutableIdentityPath() {
  std::array<char, 4097> path{};
  ssize_t length = -1;
  do {
    length = readlink("/proc/self/exe", path.data(), path.size() - 1);
  } while (length < 0 && errno == EINTR);
  if (length <= 0) return "mocktail";
  path[static_cast<std::size_t>(length)] = '\0';

  const std::filesystem::path executable = path.data();
  std::error_code executable_error;
  const std::filesystem::path canonical_executable =
      std::filesystem::weakly_canonical(executable, executable_error);
  const std::filesystem::path normalized_executable =
      executable_error ? executable.lexically_normal() : canonical_executable;

  const char* app_image_value = std::getenv("APPIMAGE");
  const char* app_dir_value = std::getenv("APPDIR");
  const std::filesystem::path app_image =
      app_image_value == nullptr ? std::filesystem::path() : app_image_value;
  const std::filesystem::path app_dir =
      app_dir_value == nullptr ? std::filesystem::path() : app_dir_value;
  if (app_image.is_absolute() && app_dir.is_absolute() &&
      !executable_error) {
    std::error_code app_dir_error;
    const std::filesystem::path canonical_app_dir =
        std::filesystem::weakly_canonical(app_dir, app_dir_error);
    const bool app_dir_is_directory =
        !app_dir_error &&
        std::filesystem::is_directory(canonical_app_dir, app_dir_error);
    if (!app_dir_error && app_dir_is_directory) {
      auto executable_part = normalized_executable.begin();
      auto app_dir_part = canonical_app_dir.begin();
      while (executable_part != normalized_executable.end() &&
             app_dir_part != canonical_app_dir.end() &&
             *executable_part == *app_dir_part) {
        ++executable_part;
        ++app_dir_part;
      }
      if (app_dir_part == canonical_app_dir.end() &&
          executable_part != normalized_executable.end()) {
        std::error_code app_image_error;
        const std::filesystem::path canonical_app_image =
            std::filesystem::weakly_canonical(app_image, app_image_error);
        return (app_image_error ? app_image.lexically_normal()
                                : canonical_app_image)
            .string();
      }
    }
  }
  return normalized_executable.string();
}

std::filesystem::path CurrentUserLockPath() {
  const uid_t user_id = getuid();
  const std::string instance_key = CurrentExecutableInstanceKey();
  const std::string filename =
      "mocktail." + instance_key + ".instance.lock";
  const std::filesystem::path runtime_directory =
      std::filesystem::path("/run/user") / std::to_string(user_id);
  struct stat directory_status{};
  if (stat(runtime_directory.c_str(), &directory_status) == 0 &&
      S_ISDIR(directory_status.st_mode) && directory_status.st_uid == user_id) {
    return runtime_directory / filename;
  }
  return std::filesystem::path("/tmp") /
         ("mocktail-" + std::to_string(user_id) + "." +
          instance_key + ".instance.lock");
}

std::string ErrnoMessage(std::string_view operation,
                         const std::filesystem::path& path, int error_number) {
  return std::string(operation) + " " + path.string() + ": " +
         std::strerror(error_number);
}

bool IsPositiveDecimal(std::string_view value) {
  if (value.empty() || value == "0") {
    return false;
  }
  return std::all_of(value.begin(), value.end(), [](unsigned char character) {
    return character >= '0' && character <= '9';
  });
}

}  // namespace

std::string CurrentExecutableInstanceKey() {
  static const std::string instance_key = []() {
    // FNV-1a is used only for a compact deterministic namespace. The endpoint
    // still requires private same-user ownership and peer credential checks;
    // this value is not an authorization token.
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char byte : ExecutableIdentityPath()) {
      hash ^= byte;
      hash *= 1099511628211ULL;
    }
    std::array<char, 17> encoded{};
    std::snprintf(encoded.data(), encoded.size(), "%016llx",
                  static_cast<unsigned long long>(hash));
    return std::string(encoded.data());
  }();
  return instance_key;
}

SingleInstanceLock::SingleInstanceLock(int descriptor, Status status,
                                       std::filesystem::path path,
                                       std::string error)
    : descriptor_(descriptor),
      status_(status),
      path_(std::move(path)),
      error_(std::move(error)) {}

SingleInstanceLock::~SingleInstanceLock() {
  if (descriptor_ >= 0) {
    close(descriptor_);
  }
}

SingleInstanceLock::SingleInstanceLock(SingleInstanceLock&& other) noexcept
    : descriptor_(other.descriptor_),
      status_(other.status_),
      path_(std::move(other.path_)),
      error_(std::move(other.error_)) {
  other.descriptor_ = -1;
}

SingleInstanceLock& SingleInstanceLock::operator=(
    SingleInstanceLock&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  if (descriptor_ >= 0) {
    close(descriptor_);
  }
  descriptor_ = other.descriptor_;
  status_ = other.status_;
  path_ = std::move(other.path_);
  error_ = std::move(other.error_);
  other.descriptor_ = -1;
  return *this;
}

SingleInstanceLock SingleInstanceLock::AcquireForCurrentUser() {
  return Acquire(CurrentUserLockPath());
}

SingleInstanceLock SingleInstanceLock::AcquireForLaunch(
    const Environment& environment, const RuntimePaths& paths) {
  const std::optional<std::string> canary =
      environment.Get("MOCKTAIL_ISOLATED_CANARY");
  if (!canary.has_value() || canary->empty() || *canary == "0") {
    return AcquireForCurrentUser();
  }
  const auto reject = [](std::string error) {
    return SingleInstanceLock(-1, Status::kError, {}, std::move(error));
  };
  if (*canary != "1" ||
      !environment.HasNonEmpty("MOCKTAIL_DATA_ROOT") ||
      !environment.HasNonEmpty("MOCKTAIL_CACHE_ROOT") ||
      !environment.HasNonEmpty("MOCKTAIL_STATE_ROOT") ||
      !environment.HasNonEmpty("ROBLOX_LIB_PATH") ||
      !environment.HasNonEmpty("MOCKTAIL_ASSET_PATH") ||
      environment.GetOr("MOCKTAIL_SKIP_UPDATE_CHECK", "0") != "1" ||
      !IsPositiveDecimal(
          environment.GetOr("MOCKTAIL_AUTO_EXIT_AFTER_PRESENT_MS", ""))) {
    return reject("isolated canary requires explicit payload, XDG roots, "
                  "update bypass, and a bounded present timer");
  }

  const std::filesystem::path data_root = paths.data_root().lexically_normal();
  const std::filesystem::path cache_root =
      paths.cache_root().lexically_normal();
  const std::filesystem::path state_root =
      paths.state_root().lexically_normal();
  if (!data_root.is_absolute() || !cache_root.is_absolute() ||
      !state_root.is_absolute() || data_root == cache_root ||
      data_root == state_root || cache_root == state_root ||
      data_root.parent_path() != cache_root.parent_path() ||
      data_root.parent_path() != state_root.parent_path()) {
    return reject(
        "isolated canary data, cache, and state roots must be distinct "
        "absolute siblings");
  }
  return Acquire(state_root / "instance.lock");
}

SingleInstanceLock SingleInstanceLock::Acquire(
    const std::filesystem::path& lock_file) {
  const int descriptor =
      open(lock_file.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (descriptor < 0) {
    const int error_number = errno;
    return SingleInstanceLock(
        -1, Status::kError, lock_file,
        ErrnoMessage("cannot open instance lock", lock_file, error_number));
  }

  struct stat file_status{};
  if (fstat(descriptor, &file_status) != 0) {
    const int error_number = errno;
    close(descriptor);
    return SingleInstanceLock(
        -1, Status::kError, lock_file,
        ErrnoMessage("cannot inspect instance lock", lock_file, error_number));
  }
  if (!S_ISREG(file_status.st_mode) || file_status.st_uid != getuid()) {
    close(descriptor);
    return SingleInstanceLock(
        -1, Status::kError, lock_file,
        "instance lock must be a regular file owned by the current user: " +
            lock_file.string());
  }
  if (fchmod(descriptor, 0600) != 0) {
    const int error_number = errno;
    close(descriptor);
    return SingleInstanceLock(
        -1, Status::kError, lock_file,
        ErrnoMessage("cannot secure instance lock", lock_file, error_number));
  }

  int lock_result = -1;
  do {
    lock_result = flock(descriptor, LOCK_EX | LOCK_NB);
  } while (lock_result != 0 && errno == EINTR);
  if (lock_result != 0) {
    const int error_number = errno;
    close(descriptor);
    if (error_number == EWOULDBLOCK || error_number == EAGAIN) {
      return SingleInstanceLock(-1, Status::kAlreadyRunning, lock_file, "");
    }
    return SingleInstanceLock(
        -1, Status::kError, lock_file,
        ErrnoMessage("cannot acquire instance lock", lock_file, error_number));
  }
  return SingleInstanceLock(descriptor, Status::kAcquired, lock_file, "");
}

}  // namespace runtime
}  // namespace mocktail
