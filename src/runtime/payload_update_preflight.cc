#include "runtime/payload_update_preflight.h"

#include <fcntl.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern char** environ;

namespace mocktail {
namespace runtime {
namespace {

constexpr int kUpdaterProgressDescriptor = 10;
constexpr char kProgressFinishedPacket = 'S';

bool Enabled(const Environment& environment, std::string_view name) {
  const auto value = environment.Get(name);
  return value.has_value() && !value->empty() && *value != "0";
}

bool HasDisplay(const Environment& environment) {
  return environment.HasNonEmpty("DISPLAY") ||
         environment.HasNonEmpty("WAYLAND_DISPLAY");
}

bool IsExecutableRegularFile(const std::filesystem::path& path) {
  std::error_code error;
  return std::filesystem::is_regular_file(path, error) &&
         !std::filesystem::is_symlink(path, error) &&
         access(path.c_str(), X_OK) == 0;
}

std::filesystem::path UpdateHelper(const Environment& environment) {
  const auto helper_override = environment.Get("MOCKTAIL_UPDATE_HELPER");
  if (helper_override.has_value()) {
    const std::filesystem::path helper(*helper_override);
    if (helper.is_absolute() && IsExecutableRegularFile(helper)) {
      return helper;
    }
    return {};
  }

  const auto project_override = environment.Get("MOCKTAIL_PROJECT_ROOT");
  if (project_override.has_value()) {
    const std::filesystem::path root(*project_override);
    if (!root.is_absolute()) {
      return {};
    }
    for (const std::filesystem::path& helper :
         {root / "build" / "mocktail_updater",
          root / "bin" / "mocktail_updater"}) {
      if (IsExecutableRegularFile(helper)) {
        return helper;
      }
    }
    return {};
  }

  std::error_code error;
  const std::filesystem::path executable =
      std::filesystem::read_symlink("/proc/self/exe", error);
  if (error || executable.empty()) {
    return {};
  }
  const std::filesystem::path executable_dir = executable.parent_path();
  const std::vector<std::filesystem::path> candidates = {
      executable_dir / "mocktail_updater",
      executable_dir.parent_path() / "libexec" / "mocktail" /
          "mocktail_updater",
      executable_dir.parent_path() / "lib" / "mocktail" / "mocktail_updater",
  };
  for (const std::filesystem::path& candidate : candidates) {
    if (IsExecutableRegularFile(candidate)) {
      return candidate;
    }
  }
  return {};
}

std::filesystem::path ProgressHelper(const Environment& environment) {
  const auto helper_override =
      environment.Get("MOCKTAIL_UPDATE_PROGRESS_HELPER");
  if (helper_override.has_value()) {
    const std::filesystem::path helper(*helper_override);
    return helper.is_absolute() && IsExecutableRegularFile(helper)
               ? helper
               : std::filesystem::path{};
  }

  std::error_code error;
  const std::filesystem::path executable =
      std::filesystem::read_symlink("/proc/self/exe", error);
  if (error || executable.empty()) {
    return {};
  }
  const std::filesystem::path helper =
      executable.parent_path() / "mocktail_failure_dialog";
  return IsExecutableRegularFile(helper) ? helper : std::filesystem::path{};
}

void WaitForProcess(pid_t process) {
  if (process <= 0) {
    return;
  }
  while (waitpid(process, nullptr, 0) < 0 && errno == EINTR) {
  }
}

class UpdateProgressMonitor final {
 public:
  UpdateProgressMonitor() = default;
  ~UpdateProgressMonitor() { Finish(); }

  UpdateProgressMonitor(const UpdateProgressMonitor&) = delete;
  UpdateProgressMonitor& operator=(const UpdateProgressMonitor&) = delete;

  static UpdateProgressMonitor Start(const Environment& environment) {
    if (Enabled(environment, "MOCKTAIL_DISABLE_UPDATE_PROGRESS") ||
        Enabled(environment, "MOCKTAIL_DISABLE_FAILURE_DIALOG") ||
        Enabled(environment, "MOCKTAIL_ISOLATED_CANARY") ||
        Enabled(environment, "MOCKTAIL_HEADLESS") || !HasDisplay(environment)) {
      return {};
    }
    const std::filesystem::path helper = ProgressHelper(environment);
    if (helper.empty()) {
      return {};
    }

    int sockets[2] = {-1, -1};
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) != 0) {
      return {};
    }
    posix_spawn_file_actions_t actions;
    if (posix_spawn_file_actions_init(&actions) != 0) {
      close(sockets[0]);
      close(sockets[1]);
      return {};
    }
    int action_status = 0;
    if (sockets[0] == STDIN_FILENO) {
      action_status = posix_spawn_file_actions_addclose(&actions, sockets[0]);
    }
    if (action_status == 0 && sockets[1] != STDIN_FILENO) {
      action_status =
          posix_spawn_file_actions_adddup2(&actions, sockets[1], STDIN_FILENO);
    }
    if (action_status == 0 && sockets[0] != STDIN_FILENO) {
      action_status = posix_spawn_file_actions_addclose(&actions, sockets[0]);
    }
    if (action_status == 0 && sockets[1] != STDIN_FILENO) {
      action_status = posix_spawn_file_actions_addclose(&actions, sockets[1]);
    }

    std::string helper_string = helper.string();
    char progress_option[] = "--progress-monitor";
    char* arguments[] = {helper_string.data(), progress_option, nullptr};
    pid_t child = -1;
    int spawn_status = action_status;
    if (spawn_status == 0) {
      spawn_status = posix_spawn(&child, helper.c_str(), &actions, nullptr,
                                 arguments, environ);
    }
    posix_spawn_file_actions_destroy(&actions);
    close(sockets[1]);
    if (spawn_status != 0) {
      close(sockets[0]);
      return {};
    }
    return UpdateProgressMonitor(sockets[0], child);
  }

  bool active() const { return socket_ >= 0; }
  int socket() const { return socket_; }

 private:
  UpdateProgressMonitor(int socket, pid_t process)
      : socket_(socket), process_(process) {}

  void Finish() {
    if (!active()) {
      return;
    }
    (void)send(socket_, &kProgressFinishedPacket,
               sizeof(kProgressFinishedPacket), MSG_NOSIGNAL);
    (void)shutdown(socket_, SHUT_RDWR);
    close(socket_);
    socket_ = -1;
    WaitForProcess(process_);
    process_ = -1;
  }

  int socket_ = -1;
  pid_t process_ = -1;
};

bool IsEnvironmentEntry(std::string_view entry, std::string_view name) {
  return entry.size() > name.size() && entry[name.size()] == '=' &&
         entry.substr(0, name.size()) == name;
}

bool IsOverriddenEnvironmentEntry(
    std::string_view entry,
    const std::vector<std::pair<std::string_view, std::filesystem::path>>&
        overrides) {
  for (const auto& [name, ignored] : overrides) {
    if (IsEnvironmentEntry(entry, name)) {
      return true;
    }
  }
  return false;
}

// The updater runs as a separate process, so the reason it failed ("api.
// pureapk.com request returned status 503") only ever reached the session log.
// Relay its stderr verbatim and keep the last failure it reported, so the
// launcher can tell the user what actually went wrong.
class UpdaterStderrRelay final {
 public:
  UpdaterStderrRelay() {
    if (pipe2(descriptors_, O_CLOEXEC) != 0) {
      descriptors_[0] = -1;
      descriptors_[1] = -1;
    }
  }

  ~UpdaterStderrRelay() {
    CloseDescriptor(&descriptors_[0]);
    CloseWrite();
  }

  UpdaterStderrRelay(const UpdaterStderrRelay&) = delete;
  UpdaterStderrRelay& operator=(const UpdaterStderrRelay&) = delete;

  bool active() const { return descriptors_[0] >= 0; }
  int write_descriptor() const { return descriptors_[1]; }
  void CloseWrite() { CloseDescriptor(&descriptors_[1]); }
  const std::string& failure() const { return failure_; }
  const MocktailUpdateNotice& notice() const { return notice_; }

  // Must run before waitpid: a child that fills the pipe blocks until it is
  // drained, and waiting first would deadlock the launcher.
  void Drain() {
    if (!active()) {
      return;
    }
    std::string pending;
    std::array<char, 4096> buffer{};
    while (true) {
      const ssize_t received =
          read(descriptors_[0], buffer.data(), buffer.size());
      if (received < 0 && errno == EINTR) {
        continue;
      }
      if (received <= 0) {
        break;
      }
      const auto bytes = static_cast<std::size_t>(received);
      WriteAll(buffer.data(), bytes);
      pending.append(buffer.data(), bytes);
      for (std::size_t end = pending.find('\n'); end != std::string::npos;
           end = pending.find('\n')) {
        RecordLine(std::string_view(pending).substr(0, end));
        pending.erase(0, end + 1);
      }
    }
    RecordLine(pending);
    CloseDescriptor(&descriptors_[0]);
  }

 private:
  // An Adwaita alert dialog stays readable well below the packet limit.
  static constexpr std::size_t kMaximumFailureBytes = 400;
  static constexpr std::size_t kMaximumNoticeFieldBytes = 1024;

  static void CloseDescriptor(int* descriptor) {
    if (*descriptor >= 0) {
      close(*descriptor);
      *descriptor = -1;
    }
  }

  static void WriteAll(const char* data, std::size_t bytes) {
    std::size_t offset = 0;
    while (offset < bytes) {
      const ssize_t written =
          write(STDERR_FILENO, data + offset, bytes - offset);
      if (written < 0 && errno == EINTR) {
        continue;
      }
      if (written <= 0) {
        return;
      }
      offset += static_cast<std::size_t>(written);
    }
  }

  // A message cut mid-codepoint fails UTF-8 validation in the dialog helper
  // and would be replaced by the generic text.
  static std::string_view TrimToCharacterBoundary(
      std::string_view text, std::size_t maximum = kMaximumFailureBytes) {
    if (text.size() <= maximum) {
      return text;
    }
    std::size_t size = maximum;
    while (size > 0 &&
           (static_cast<unsigned char>(text[size]) & 0xC0U) == 0x80U) {
      --size;
    }
    return text.substr(0, size);
  }

  static void AppendNoticeLine(std::string* field, std::string_view line) {
    std::string combined = *field;
    if (!combined.empty()) combined.push_back('\n');
    combined.append(line);
    field->assign(TrimToCharacterBoundary(combined, kMaximumNoticeFieldBytes));
  }

  bool RecordNoticeLine(std::string_view detail) {
    const std::pair<std::string_view, std::string*> fields[] = {
        {"notice-heading: ", &notice_.heading},
        {"notice-command: ", &notice_.command},
        {"notice: ", &notice_.body},
    };
    for (const auto& [prefix, field] : fields) {
      if (detail.substr(0, prefix.size()) == prefix) {
        AppendNoticeLine(field, detail.substr(prefix.size()));
        return true;
      }
    }
    return false;
  }

  void RecordLine(std::string_view line) {
    constexpr std::string_view kPrefix = "[native-updater] ";
    constexpr std::string_view kWarningPrefix = "warning: ";
    if (line.size() <= kPrefix.size() ||
        line.substr(0, kPrefix.size()) != kPrefix) {
      return;
    }
    const std::string_view detail = line.substr(kPrefix.size());
    if (detail.substr(0, kWarningPrefix.size()) == kWarningPrefix ||
        RecordNoticeLine(detail)) {
      return;
    }
    failure_.assign(TrimToCharacterBoundary(detail));
  }

  int descriptors_[2] = {-1, -1};
  std::string failure_;
  MocktailUpdateNotice notice_;
};

std::vector<std::string> ChildEnvironment(const RuntimePaths& paths,
                                          bool progress_enabled) {
  const std::vector<std::pair<std::string_view, std::filesystem::path>>
      overrides = {
          {"MOCKTAIL_CONFIG_FILE", paths.config_file()},
          {"MOCKTAIL_CONFIG_ROOT", paths.config_root()},
          {"MOCKTAIL_DATA_ROOT", paths.data_root()},
          {"MOCKTAIL_CACHE_ROOT", paths.cache_root()},
          {"MOCKTAIL_STATE_ROOT", paths.state_root()},
      };
  std::vector<std::string> child_environment;
  for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
    if (!IsOverriddenEnvironmentEntry(*entry, overrides) &&
        !IsEnvironmentEntry(*entry, "MOCKTAIL_UPDATE_PROGRESS_FD")) {
      child_environment.emplace_back(*entry);
    }
  }
  for (const auto& [name, value] : overrides) {
    child_environment.emplace_back(std::string(name) + "=" + value.string());
  }
  if (progress_enabled) {
    child_environment.emplace_back("MOCKTAIL_UPDATE_PROGRESS_FD=" +
                                   std::to_string(kUpdaterProgressDescriptor));
  }
  return child_environment;
}

}  // namespace

PayloadUpdatePreflightResult RunPayloadUpdatePreflight(
    const Environment& environment, const RuntimePaths& paths,
    bool force_run_latest) {
  PayloadUpdatePreflightResult result;
  if (!force_run_latest &&
      (Enabled(environment, "MOCKTAIL_SKIP_UPDATE_CHECK") ||
       environment.HasNonEmpty("ROBLOX_LIB_PATH"))) {
    return result;
  }

  const std::filesystem::path helper = UpdateHelper(environment);
  if (helper.empty()) {
    if (force_run_latest) {
      result.error = "cannot locate the Mocktail update helper";
    }
    return result;
  }

  result.attempted = true;
  std::string helper_string = helper.string();
  char update[] = "update";
  char startup_preflight[] = "--startup-preflight";
  char force_latest[] = "--force-run-latest";
  char* arguments[] = {helper_string.data(), update,
                       force_run_latest ? force_latest : startup_preflight,
                       nullptr};
  UpdateProgressMonitor progress = UpdateProgressMonitor::Start(environment);
  std::vector<std::string> child_environment =
      ChildEnvironment(paths, progress.active());
  std::vector<char*> child_environment_pointers;
  child_environment_pointers.reserve(child_environment.size() + 1);
  for (std::string& entry : child_environment) {
    child_environment_pointers.push_back(entry.data());
  }
  child_environment_pointers.push_back(nullptr);
  UpdaterStderrRelay stderr_relay;
  posix_spawn_file_actions_t actions;
  bool actions_initialized = false;
  int action_status = 0;
  if (progress.active() || stderr_relay.active()) {
    action_status = posix_spawn_file_actions_init(&actions);
    actions_initialized = action_status == 0;
  }
  if (action_status == 0 && progress.active()) {
    action_status = posix_spawn_file_actions_adddup2(
        &actions, progress.socket(), kUpdaterProgressDescriptor);
  }
  if (action_status == 0 && stderr_relay.active()) {
    action_status = posix_spawn_file_actions_adddup2(
        &actions, stderr_relay.write_descriptor(), STDERR_FILENO);
  }
  if (action_status != 0) {
    if (actions_initialized) {
      posix_spawn_file_actions_destroy(&actions);
    }
    result.error = "cannot prepare Roblox update progress channel: " +
                   std::string(std::strerror(action_status));
    return result;
  }
  pid_t child = -1;
  const int spawn_status = posix_spawn(
      &child, helper.c_str(), actions_initialized ? &actions : nullptr, nullptr,
      arguments, child_environment_pointers.data());
  if (actions_initialized) {
    posix_spawn_file_actions_destroy(&actions);
  }
  if (spawn_status != 0) {
    result.error = "cannot start Roblox update preflight: " +
                   std::string(std::strerror(spawn_status));
    return result;
  }

  // The child owns the write end now; the relay must observe end-of-file.
  stderr_relay.CloseWrite();
  stderr_relay.Drain();
  result.notice = stderr_relay.notice();

  int child_status = 0;
  while (waitpid(child, &child_status, 0) < 0) {
    if (errno == EINTR) {
      continue;
    }
    result.error = "cannot wait for Roblox update preflight: " +
                   std::string(std::strerror(errno));
    return result;
  }
  if (!WIFEXITED(child_status) || WEXITSTATUS(child_status) != 0) {
    result.error = force_run_latest ? "forced latest Roblox run failed"
                                     : "Roblox update preflight failed";
    result.details = stderr_relay.failure();
  }
  return result;
}

}  // namespace runtime
}  // namespace mocktail
