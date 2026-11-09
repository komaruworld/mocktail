#include "runtime/roblox_cookie_store.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include "runtime/helper_environment.h"
#include "runtime/runtime_config_file.h"
#include "runtime/runtime_paths.h"

extern char** environ;

#ifndef MOCKTAIL_INSTALL_LIBDIR
#define MOCKTAIL_INSTALL_LIBDIR "lib"
#endif

namespace mocktail::runtime {
namespace {

constexpr size_t kMaximumCookieBytes = 1024 * 1024;
std::atomic<void (*)(std::string_view)> warning_handler{nullptr};
std::atomic<bool> warning_shown{false};

void ClearString(std::string* value) {
  volatile char* bytes = value->empty() ? nullptr : value->data();
  for (size_t i = 0; i < value->size(); ++i) bytes[i] = '\0';
  value->clear();
}

class NotifyingSecretStore : public RobloxSecretStore {
 public:
  void Warn(std::string_view message) override {
    RobloxSecretStore::Warn(message);
    if (warning_shown.exchange(true)) return;
    if (const auto handler = warning_handler.load()) handler(message);
  }
};

class DisabledSecretStore final : public NotifyingSecretStore {
 public:
  bool enabled() const override { return false; }
  StoredCookie Load(const std::filesystem::path&) override { return {}; }
};

std::filesystem::path CookieHelper() {
  std::error_code error;
  const auto executable =
      std::filesystem::read_symlink("/proc/self/exe", error);
  if (error) return {};
  const auto directory = executable.parent_path();
  const char* override = std::getenv("MOCKTAIL_COOKIE_HELPER");
  std::vector<std::filesystem::path> candidates;
  if (override != nullptr) {
    candidates.emplace_back(override);
  } else {
    candidates = {
        directory / "mocktail_cookie_helper",
        directory.parent_path() / MOCKTAIL_INSTALL_LIBDIR /
            "mocktail/mocktail_cookie_helper",
        directory.parent_path() / "libexec/mocktail/mocktail_cookie_helper"};
    if (directory.filename() == "tests")
      candidates.push_back(directory.parent_path() / "mocktail_cookie_helper");
  }
  for (const auto& path : candidates) {
    if (path.is_absolute() && std::filesystem::is_regular_file(path, error) &&
        access(path.c_str(), X_OK) == 0)
      return path;
  }
  return {};
}

std::string ExchangeCookie(char operation, const std::filesystem::path& profile,
                           std::string_view cookie = {}) {
  const auto helper = CookieHelper();
  std::error_code error;
  const auto key =
      std::filesystem::absolute(profile, error).lexically_normal().string();
  if (helper.empty() || error || key.find_first_of("\r\n") != key.npos ||
      key.find('\0') != key.npos || key.size() > 4096 ||
      cookie.size() > kMaximumCookieBytes)
    return {};
  int sockets[2];
  if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) != 0)
    return {};
  for (int& fd : sockets) {
    if (fd < 3) {
      const int moved = fcntl(fd, F_DUPFD_CLOEXEC, 3);
      close(fd);
      fd = moved;
    }
  }
  posix_spawn_file_actions_t actions;
  int status = posix_spawn_file_actions_init(&actions);
  if (status != 0 || sockets[0] < 0 || sockets[1] < 0) {
    close(sockets[0]);
    close(sockets[1]);
    if (status == 0) posix_spawn_file_actions_destroy(&actions);
    return {};
  }
  status = posix_spawn_file_actions_adddup2(&actions, sockets[1], STDIN_FILENO);
  if (status == 0)
    status =
        posix_spawn_file_actions_adddup2(&actions, sockets[1], STDOUT_FILENO);
  if (status == 0)
    status = posix_spawn_file_actions_addclose(&actions, sockets[0]);
  if (status == 0)
    status = posix_spawn_file_actions_addclose(&actions, sockets[1]);
  std::string program = helper.string();
  char* arguments[] = {program.data(), nullptr};
  const auto environment = BuildHelperEnvironment(environ);
  pid_t child = -1;
  if (status == 0)
    status = posix_spawn(&child, program.c_str(), &actions, nullptr, arguments,
                         environment.data());
  posix_spawn_file_actions_destroy(&actions);
  close(sockets[1]);
  if (status != 0) {
    close(sockets[0]);
    return {};
  }
  std::string request(1, operation);
  request += "\n" + key + "\n";
  request.append(cookie);
  std::string response;
  size_t sent = 0;
  bool eof = false;
  bool exited = false;
  int child_status = 0;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (std::chrono::steady_clock::now() < deadline && !(eof && exited)) {
    if (!exited) {
      const pid_t reaped = waitpid(child, &child_status, WNOHANG);
      exited = reaped == child;
      if (reaped < 0 && errno != EINTR) {
        exited = true;
        child_status = 1 << 8;
        break;
      }
    }
    if (eof) {
      (void)poll(nullptr, 0, 20);
      continue;
    }
    pollfd socket{
        sockets[0],
        static_cast<short>(POLLIN | (sent < request.size() ? POLLOUT : 0)), 0};
    if (poll(&socket, 1, 20) < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if ((socket.revents & POLLOUT) && sent < request.size()) {
      const ssize_t count =
          send(sockets[0], request.data() + sent, request.size() - sent,
               MSG_NOSIGNAL | MSG_DONTWAIT);
      if (count > 0) {
        sent += static_cast<size_t>(count);
        if (sent == request.size()) shutdown(sockets[0], SHUT_WR);
      } else if (count < 0 && errno != EINTR && errno != EAGAIN)
        break;
    }
    if (socket.revents & (POLLIN | POLLHUP | POLLERR)) {
      std::array<char, 4096> buffer{};
      const ssize_t count =
          recv(sockets[0], buffer.data(), buffer.size(), MSG_DONTWAIT);
      if (count == 0)
        eof = true;
      else if (count > 0)
        response.append(buffer.data(), static_cast<size_t>(count));
      else if (errno != EINTR && errno != EAGAIN)
        break;
      volatile char* bytes = buffer.data();
      for (size_t i = 0; i < buffer.size(); ++i) bytes[i] = '\0';
      if (response.size() > kMaximumCookieBytes + 1) break;
    }
  }
  close(sockets[0]);
  ClearString(&request);
  if (!exited) {
    kill(child, SIGKILL);
    while (waitpid(child, &child_status, 0) < 0 && errno == EINTR) {
    }
  }
  if (!exited || !eof || !WIFEXITED(child_status) ||
      WEXITSTATUS(child_status) != 0 ||
      response.size() > kMaximumCookieBytes + 1)
    ClearString(&response);
  return response;
}

class HelperSecretStore final : public NotifyingSecretStore {
 public:
  StoredCookie Load(const std::filesystem::path& profile) override {
    if (!available_) return RobloxSecretStore::Load(profile);
    std::string response = ExchangeCookie('L', profile);
    StoredCookie result;
    result.from_secret_store = true;
    if (!response.empty() && response.front() == 'F') {
      result.status = StoredCookieStatus::kFound;
      result.value = response.substr(1);
    } else if (response != "M") {
      available_ = false;
      result = RobloxSecretStore::Load(profile);
    }
    ClearString(&response);
    return result;
  }
  bool Store(const std::filesystem::path& profile,
             const std::string& cookie) override {
    if (!available_) return false;
    const bool stored = ExchangeCookie('S', profile, cookie) == "F";
    if (!stored) available_ = false;
    return stored;
  }
  bool Clear(const std::filesystem::path& profile) override {
    return available_ && ExchangeCookie('C', profile) == "F";
  }

 private:
  std::atomic<bool> available_{true};
};

}  // namespace

StoredCookie RobloxSecretStore::Load(const std::filesystem::path&) {
  return {StoredCookieStatus::kUnavailable,
          {},
          "secure Roblox cookie storage is unavailable",
          true};
}

bool RobloxSecretStore::Store(const std::filesystem::path&,
                              const std::string&) {
  return false;
}
bool RobloxSecretStore::Clear(const std::filesystem::path&) { return false; }

void RobloxSecretStore::Warn(std::string_view message) {
  std::fprintf(stderr, "  [auth] %.*s\n", static_cast<int>(message.size()),
               message.data());
}

std::shared_ptr<RobloxSecretStore> OpenRobloxSecretStore() {
  return OpenRobloxSecretStore(ProcessEnvironment());
}

std::shared_ptr<RobloxSecretStore> OpenRobloxSecretStore(
    const Environment& environment) {
  const auto paths = RuntimePaths::FromEnvironment(environment);
  const auto loaded = LoadRuntimeConfig(environment, paths.config_file());
  if (loaded && loaded.config.use_libsecret())
    return std::make_shared<HelperSecretStore>();
  return std::make_shared<DisabledSecretStore>();
}

void SetRobloxCookieWarningHandler(void (*handler)(std::string_view)) {
  warning_handler.store(handler);
}

StoredCookie ReadRobloxCookieFile(const std::filesystem::path& path,
                                  bool required) {
  const int descriptor =
      open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    if (errno == ENOENT && !required) return {};
    return {StoredCookieStatus::kUnavailable,
            {},
            "configured Roblox cookie file is unavailable"};
  }
  struct CloseFile {
    int descriptor;
    ~CloseFile() { close(descriptor); }
  } file{descriptor};
  struct stat metadata{};
  if (fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
      metadata.st_uid != geteuid())
    return {StoredCookieStatus::kUnavailable,
            {},
            "Roblox cookie source is not a regular file"};
  if ((metadata.st_mode & (S_IRWXG | S_IRWXO)) != 0)
    return {StoredCookieStatus::kUnavailable,
            {},
            "Roblox cookie file permissions are not private"};
  if (metadata.st_size < 0 ||
      static_cast<uintmax_t>(metadata.st_size) > kMaximumCookieBytes)
    return {StoredCookieStatus::kUnavailable,
            {},
            "Roblox cookie file exceeds the safe read limit"};

  StoredCookie result{StoredCookieStatus::kFound, {}, {}};
  std::array<char, 4096> buffer{};
  while (true) {
    const ssize_t count = read(descriptor, buffer.data(), buffer.size());
    if (count == 0) break;
    if (count < 0 && errno == EINTR) continue;
    if (count < 0 || result.value.size() + static_cast<size_t>(count) >
                         kMaximumCookieBytes) {
      result.error = "Roblox cookie file could not be read safely";
      break;
    }
    result.value.append(buffer.data(), static_cast<size_t>(count));
  }
  volatile char* bytes = buffer.data();
  for (size_t i = 0; i < buffer.size(); ++i) bytes[i] = '\0';
  struct stat after{};
  if (fstat(descriptor, &after) != 0 || metadata.st_size != after.st_size ||
      metadata.st_mtim.tv_sec != after.st_mtim.tv_sec ||
      metadata.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
      metadata.st_ctim.tv_sec != after.st_ctim.tv_sec ||
      metadata.st_ctim.tv_nsec != after.st_ctim.tv_nsec ||
      static_cast<uintmax_t>(after.st_size) != result.value.size())
    result.error = "Roblox cookie file changed while it was read";
  if (required && result.value.empty())
    result.error = "configured Roblox cookie file is empty";
  if (!result.error.empty()) {
    ClearString(&result.value);
    result.status = StoredCookieStatus::kUnavailable;
  }
  return result;
}

StoredCookie LoadManagedRobloxCookie(const std::filesystem::path& path,
                                     RobloxSecretStore& secrets) {
  StoredCookie file = ReadRobloxCookieFile(path);
  return file.status == StoredCookieStatus::kMissing ? secrets.Load(path)
                                                     : std::move(file);
}

bool RemoveLegacyWebViewCookies(const std::filesystem::path& directory) {
  if (directory.empty()) return true;
  const int fd =
      open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return errno == ENOENT;
  struct stat metadata{};
  bool removed = fstat(fd, &metadata) == 0 && metadata.st_uid == geteuid();
  for (const char* name : {"cookies.sqlite", "cookies.sqlite-wal",
                           "cookies.sqlite-shm", "cookies.sqlite-journal"}) {
    if (!removed) break;
    if (fstatat(fd, name, &metadata, AT_SYMLINK_NOFOLLOW) != 0) {
      removed = errno == ENOENT;
      continue;
    }
    removed = S_ISREG(metadata.st_mode) && metadata.st_uid == geteuid() &&
              metadata.st_nlink == 1 && unlinkat(fd, name, 0) == 0;
  }
  if (removed) removed = fsync(fd) == 0;
  close(fd);
  return removed;
}

}  // namespace mocktail::runtime
