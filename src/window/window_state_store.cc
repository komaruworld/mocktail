#include "window/window_state_store.h"

#define JSON_NOEXCEPTION 1
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>

namespace mocktail {
namespace window {
namespace {

constexpr int kSchemaVersion = 1;
constexpr std::uintmax_t kMaximumStateBytes = 32U * 1024U;
constexpr int kMaximumExtent = 16384;
constexpr int kMaximumCoordinateMagnitude = 131072;
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

Status InvalidState(std::string message) {
  return Status::Error(StatusCode::kInvalidArgument, std::move(message));
}

Status PlatformError(std::string message) {
  return Status::Error(StatusCode::kPlatformError, std::move(message));
}

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

bool IsValidState(const PersistedWindowState& state) {
  return state.width >= kMinimumWindowWidth &&
         state.width <= kMaximumExtent &&
         state.height >= kMinimumWindowHeight &&
         state.height <= kMaximumExtent &&
         (!state.has_position || (state.x >= -kMaximumCoordinateMagnitude &&
                                  state.x <= kMaximumCoordinateMagnitude &&
                                  state.y >= -kMaximumCoordinateMagnitude &&
                                  state.y <= kMaximumCoordinateMagnitude));
}

bool ReadInteger(const nlohmann::json& object, const char* name, int* value) {
  const auto found = object.find(name);
  if (found == object.end() || !found->is_number_integer()) {
    return false;
  }
  const std::int64_t decoded = found->get<std::int64_t>();
  if (decoded < std::numeric_limits<int>::min() ||
      decoded > std::numeric_limits<int>::max()) {
    return false;
  }
  *value = static_cast<int>(decoded);
  return true;
}

bool ReadBoolean(const nlohmann::json& object, const char* name, bool* value) {
  const auto found = object.find(name);
  if (found == object.end() || !found->is_boolean()) {
    return false;
  }
  *value = found->get<bool>();
  return true;
}

}  // namespace

WindowStateLoadResult LoadWindowState(const std::filesystem::path& path) {
  WindowStateLoadResult result;
  if (!path.is_absolute()) {
    result.status = InvalidState("window state path must be absolute");
    return result;
  }

  const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    if (errno == ENOENT) {
      return result;
    }
    result.status = PlatformError("cannot safely open window state");
    return result;
  }
  const ScopedFileDescriptor file(descriptor);
  struct stat file_status{};
  if (fstat(file.get(), &file_status) != 0 || !S_ISREG(file_status.st_mode) ||
      file_status.st_size < 0 ||
      static_cast<std::uintmax_t>(file_status.st_size) > kMaximumStateBytes) {
    result.status = InvalidState("window state is not a bounded regular file");
    return result;
  }

  std::string bytes(static_cast<std::size_t>(file_status.st_size), '\0');
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t count =
        read(file.get(), bytes.data() + offset, bytes.size() - offset);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      result.status = PlatformError("cannot read window state");
      return result;
    }
    if (count == 0) {
      result.status = InvalidState("window state changed while being read");
      return result;
    }
    offset += static_cast<std::size_t>(count);
  }

  const nlohmann::json parsed =
      nlohmann::json::parse(bytes, nullptr, false, true);
  if (parsed.is_discarded() || !parsed.is_object()) {
    result.status = InvalidState("window state is not a JSON object");
    return result;
  }
  int schema_version = 0;
  const auto windowed = parsed.find("windowed");
  if (!ReadInteger(parsed, "schema_version", &schema_version) ||
      schema_version != kSchemaVersion || windowed == parsed.end() ||
      !windowed->is_object() ||
      !ReadInteger(*windowed, "width", &result.state.width) ||
      !ReadInteger(*windowed, "height", &result.state.height) ||
      !ReadBoolean(*windowed, "has_position", &result.state.has_position) ||
      !ReadBoolean(parsed, "fullscreen", &result.state.fullscreen) ||
      !ReadBoolean(parsed, "maximized", &result.state.maximized)) {
    result.status = InvalidState("window state has an unsupported schema");
    return result;
  }
  if (result.state.has_position &&
      (!ReadInteger(*windowed, "x", &result.state.x) ||
       !ReadInteger(*windowed, "y", &result.state.y))) {
    result.status = InvalidState("window state position is incomplete");
    return result;
  }
  if (!IsValidState(result.state)) {
    result.status = InvalidState("window state geometry is out of range");
    return result;
  }
  result.found = true;
  return result;
}

Status StoreWindowState(const std::filesystem::path& path,
                        const PersistedWindowState& state) {
  if (!path.is_absolute()) {
    return InvalidState("window state path must be absolute");
  }
  if (!IsValidState(state)) {
    return InvalidState("window state geometry is out of range");
  }

  std::error_code filesystem_error;
  if (!std::filesystem::create_directories(path.parent_path(),
                                           filesystem_error) &&
      filesystem_error) {
    return PlatformError("cannot create window state directory");
  }
  const std::filesystem::file_status target_status =
      std::filesystem::symlink_status(path, filesystem_error);
  if (!filesystem_error && std::filesystem::is_symlink(target_status)) {
    return InvalidState("window state target is a symlink");
  }
  if (filesystem_error != std::errc::no_such_file_or_directory &&
      filesystem_error) {
    return PlatformError("cannot inspect window state target");
  }

  nlohmann::json windowed = {
      {"has_position", state.has_position},
      {"width", state.width},
      {"height", state.height},
  };
  if (state.has_position) {
    windowed["x"] = state.x;
    windowed["y"] = state.y;
  }
  const nlohmann::json encoded = {
      {"schema_version", kSchemaVersion},
      {"fullscreen", state.fullscreen},
      {"maximized", state.maximized},
      {"windowed", std::move(windowed)},
  };
  const std::string bytes = encoded.dump();

  const std::filesystem::path temporary =
      path.parent_path() /
      (".mocktail-window-state.tmp." + std::to_string(getpid()) + "." +
       std::to_string(g_temporary_sequence.fetch_add(1)));
  const int descriptor =
      open(temporary.c_str(),
           O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (descriptor < 0) {
    return PlatformError("cannot create temporary window state");
  }
  bool stored = false;
  {
    const ScopedFileDescriptor file(descriptor);
    stored = fchmod(file.get(), S_IRUSR | S_IWUSR) == 0 &&
             WriteAll(file.get(), bytes) && fsync(file.get()) == 0;
  }
  if (!stored || rename(temporary.c_str(), path.c_str()) != 0) {
    const int saved_errno = errno;
    (void)unlink(temporary.c_str());
    errno = saved_errno;
    return PlatformError("cannot atomically store window state");
  }

  const int directory_descriptor =
      open(path.parent_path().c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
  if (directory_descriptor < 0) {
    return PlatformError("cannot open window state directory for sync");
  }
  const ScopedFileDescriptor directory(directory_descriptor);
  if (fsync(directory.get()) != 0) {
    return PlatformError("cannot sync window state directory");
  }
  return Status::Ok();
}

}  // namespace window
}  // namespace mocktail
