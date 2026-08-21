#include "runtime/failure_dialog.h"

#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

extern char** environ;

#ifndef MOCKTAIL_INSTALL_LIBDIR
#define MOCKTAIL_INSTALL_LIBDIR "lib"
#endif

namespace mocktail {
namespace runtime {
namespace {

constexpr char kMessagePacket = 'M';
constexpr char kFailurePacket = 'F';
constexpr char kSuccessPacket = 'S';
constexpr std::size_t kMaximumMessageBytes = 2048;

bool Enabled(const Environment& environment, std::string_view name) {
  const std::optional<std::string> value = environment.Get(name);
  return value.has_value() && !value->empty() && *value != "0";
}

bool HasNonEmpty(const Environment& environment, std::string_view name) {
  const std::optional<std::string> value = environment.Get(name);
  return value.has_value() && !value->empty();
}

bool IsExecutableRegularFile(const std::filesystem::path& path) {
  std::error_code error;
  return std::filesystem::is_regular_file(path, error) &&
         !std::filesystem::is_symlink(path, error) &&
         access(path.c_str(), X_OK) == 0;
}

std::filesystem::path DialogHelper(const Environment& environment) {
  const std::optional<std::string> helper_override =
      environment.Get("MOCKTAIL_FAILURE_DIALOG_HELPER");
  if (helper_override.has_value()) {
    const std::filesystem::path helper(*helper_override);
    return helper.is_absolute() && IsExecutableRegularFile(helper)
               ? helper
               : std::filesystem::path{};
  }

  const std::optional<std::string> project_override =
      environment.Get("MOCKTAIL_PROJECT_ROOT");
  if (project_override.has_value()) {
    const std::filesystem::path root(*project_override);
    if (root.is_absolute()) {
      const std::filesystem::path helper =
          root / "bin" / "mocktail_failure_dialog";
      if (IsExecutableRegularFile(helper)) {
        return helper;
      }
    }
  }

  std::error_code error;
  const std::filesystem::path executable =
      std::filesystem::read_symlink("/proc/self/exe", error);
  if (error || executable.empty()) {
    return {};
  }
  const std::filesystem::path executable_dir = executable.parent_path();
  const std::vector<std::filesystem::path> candidates = {
      executable_dir / "mocktail_failure_dialog",
      executable_dir.parent_path() / MOCKTAIL_INSTALL_LIBDIR / "mocktail" /
          "mocktail_failure_dialog",
      executable_dir.parent_path() / "libexec" / "mocktail" /
          "mocktail_failure_dialog",
  };
  for (const std::filesystem::path& candidate : candidates) {
    if (IsExecutableRegularFile(candidate)) {
      return candidate;
    }
  }
  return {};
}

void WaitForHelper(int helper_pid) {
  if (helper_pid <= 0) {
    return;
  }
  while (waitpid(helper_pid, nullptr, 0) < 0 && errno == EINTR) {
  }
}

bool SpawnOneShot(const std::filesystem::path& helper,
                  std::string_view option,
                  std::string_view message) {
  std::string helper_string = helper.string();
  std::string option_string(option);
  std::string message_string(message.substr(0, kMaximumMessageBytes));
  char* arguments[] = {helper_string.data(), option_string.data(),
                       message_string.data(), nullptr};
  pid_t child = -1;
  const int spawn_status =
      posix_spawn(&child, helper.c_str(), nullptr, nullptr, arguments, environ);
  if (spawn_status != 0) {
    return false;
  }
  WaitForHelper(child);
  return true;
}

std::pair<int, int> SpawnMonitor(const std::filesystem::path& helper) {
  int sockets[2] = {-1, -1};
  if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) != 0) {
    return {-1, -1};
  }

  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0) {
    close(sockets[0]);
    close(sockets[1]);
    return {-1, -1};
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
  char monitor_option[] = "--monitor";
  char* arguments[] = {helper_string.data(), monitor_option, nullptr};
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
    return {-1, -1};
  }
  return {sockets[0], static_cast<int>(child)};
}

bool SendPacket(int socket, char kind, std::string_view payload = {}) {
  std::string packet(1, kind);
  packet.append(payload.substr(0, kMaximumMessageBytes));
  const ssize_t sent = send(socket, packet.data(), packet.size(), MSG_NOSIGNAL);
  return sent == static_cast<ssize_t>(packet.size());
}

}  // namespace

bool FailureDialogsEnabled(const Environment& environment) {
  if (Enabled(environment, "MOCKTAIL_DISABLE_FAILURE_DIALOG") ||
      Enabled(environment, "MOCKTAIL_ISOLATED_CANARY") ||
      Enabled(environment, "MOCKTAIL_HEADLESS")) {
    return false;
  }
  return HasNonEmpty(environment, "DISPLAY") ||
         HasNonEmpty(environment, "WAYLAND_DISPLAY");
}

bool ShowFailureDialog(const Environment& environment,
                       std::string_view message) {
  if (!FailureDialogsEnabled(environment)) {
    return false;
  }
  const std::filesystem::path helper = DialogHelper(environment);
  return !helper.empty() && SpawnOneShot(helper, "--message", message);
}

bool ShowWarningDialog(const Environment& environment,
                       std::string_view message) {
  if (!FailureDialogsEnabled(environment)) {
    return false;
  }
  const std::filesystem::path helper = DialogHelper(environment);
  return !helper.empty() && SpawnOneShot(helper, "--warning", message);
}

FailureDialogMonitor::FailureDialogMonitor(int socket, int helper_pid)
    : socket_(socket), helper_pid_(helper_pid) {}

FailureDialogMonitor::~FailureDialogMonitor() { Finish(kFailurePacket); }

FailureDialogMonitor::FailureDialogMonitor(
    FailureDialogMonitor&& other) noexcept
    : socket_(std::exchange(other.socket_, -1)),
      helper_pid_(std::exchange(other.helper_pid_, -1)) {}

FailureDialogMonitor& FailureDialogMonitor::operator=(
    FailureDialogMonitor&& other) noexcept {
  if (this == &other) {
    return *this;
  }
  Finish(kFailurePacket);
  socket_ = std::exchange(other.socket_, -1);
  helper_pid_ = std::exchange(other.helper_pid_, -1);
  return *this;
}

FailureDialogMonitor FailureDialogMonitor::Start(
    const Environment& environment, std::string_view initial_message) {
  if (!FailureDialogsEnabled(environment)) {
    return {};
  }
  const std::filesystem::path helper = DialogHelper(environment);
  if (helper.empty()) {
    return {};
  }
  const auto [socket, helper_pid] = SpawnMonitor(helper);
  FailureDialogMonitor monitor(socket, helper_pid);
  if (monitor.active()) {
    monitor.SetMessage(initial_message);
  }
  return monitor;
}

void FailureDialogMonitor::SetMessage(std::string_view message) {
  if (!active() || message.empty()) {
    return;
  }
  if (!SendPacket(socket_, kMessagePacket, message)) {
    close(socket_);
    socket_ = -1;
    WaitForHelper(helper_pid_);
    helper_pid_ = -1;
  }
}

void FailureDialogMonitor::MarkSuccessful() { Finish(kSuccessPacket); }

void FailureDialogMonitor::Finish(char disposition) {
  if (!active()) {
    return;
  }
  (void)SendPacket(socket_, disposition);
  (void)shutdown(socket_, SHUT_RDWR);
  close(socket_);
  socket_ = -1;
  WaitForHelper(helper_pid_);
  helper_pid_ = -1;
}

}  // namespace runtime
}  // namespace mocktail
