#include "update/state_file.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <string>
#include <vector>

namespace mocktail::update {

nlohmann::json ReadStateFile(const std::filesystem::path& path) {
  const int descriptor =
      open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (descriptor < 0) return nullptr;
  struct stat status{};
  if (fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_size > 65536) {
    close(descriptor);
    return nullptr;
  }
  std::string contents;
  std::array<char, 4096> buffer{};
  for (;;) {
    const ssize_t size = read(descriptor, buffer.data(), buffer.size());
    if (size < 0 && errno == EINTR) continue;
    if (size < 0 || contents.size() + (size > 0 ? size : 0) > 65536) {
      close(descriptor);
      return nullptr;
    }
    if (size == 0) break;
    contents.append(buffer.data(), static_cast<std::size_t>(size));
  }
  close(descriptor);
  auto document = nlohmann::json::parse(contents, nullptr, false);
  return document.is_object() ? document : nlohmann::json(nullptr);
}

bool WriteStateFile(const std::filesystem::path& path,
                    const nlohmann::json& document) {
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) return false;
  const std::string pattern = path.string() + ".XXXXXX";
  std::vector<char> temporary(pattern.begin(), pattern.end());
  temporary.push_back('\0');
  const int descriptor = mkstemp(temporary.data());
  if (descriptor < 0) return false;
  const std::string contents = document.dump(2) + "\n";
  std::size_t offset = 0;
  while (offset < contents.size()) {
    const ssize_t size =
        write(descriptor, contents.data() + offset, contents.size() - offset);
    if (size < 0 && errno == EINTR) continue;
    if (size <= 0) break;
    offset += static_cast<std::size_t>(size);
  }
  bool success = offset == contents.size() && fsync(descriptor) == 0;
  close(descriptor);
  if (success) success = rename(temporary.data(), path.c_str()) == 0;
  if (!success) unlink(temporary.data());
  if (success) {
    const int directory =
        open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    success = directory >= 0 && fsync(directory) == 0;
    if (directory >= 0) close(directory);
  }
  return success;
}

}
