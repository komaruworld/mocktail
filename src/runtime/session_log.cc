#include "runtime/session_log.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef MOCKTAIL_PROJECT_VERSION
#define MOCKTAIL_PROJECT_VERSION "unknown"
#endif
#ifndef MOCKTAIL_BUILD_GIT_COMMIT
#define MOCKTAIL_BUILD_GIT_COMMIT "unknown"
#endif
#ifndef MOCKTAIL_BUILD_GIT_DIRTY
#define MOCKTAIL_BUILD_GIT_DIRTY "unknown"
#endif
#ifndef MOCKTAIL_BUILD_TYPE
#define MOCKTAIL_BUILD_TYPE "unknown"
#endif
#ifndef MOCKTAIL_BUILD_COMPILER_ID
#define MOCKTAIL_BUILD_COMPILER_ID "unknown"
#endif
#ifndef MOCKTAIL_BUILD_COMPILER_VERSION
#define MOCKTAIL_BUILD_COMPILER_VERSION "unknown"
#endif
#ifndef MOCKTAIL_BUILD_TARGET_SYSTEM
#define MOCKTAIL_BUILD_TARGET_SYSTEM "unknown"
#endif
#ifndef MOCKTAIL_BUILD_TARGET_PROCESSOR
#define MOCKTAIL_BUILD_TARGET_PROCESSOR "unknown"
#endif

namespace mocktail {
namespace runtime {
namespace {

bool Enabled(const Environment& environment, std::string_view name) {
  const auto value = environment.Get(name);
  return value.has_value() && !value->empty() && *value != "0";
}

std::string LocalTimestamp(std::chrono::system_clock::time_point timestamp,
                           std::string_view format) {
  const std::time_t raw = std::chrono::system_clock::to_time_t(timestamp);
  std::tm local = {};
  if (localtime_r(&raw, &local) == nullptr) return "unknown";
  std::array<char, 64> output{};
  const std::string owned_format(format);
  if (std::strftime(output.data(), output.size(), owned_format.c_str(),
                    &local) == 0) {
    return "unknown";
  }
  return output.data();
}

std::string IsoLocalTimestamp(std::chrono::system_clock::time_point timestamp) {
  std::string formatted = LocalTimestamp(timestamp, "%Y-%m-%dT%H:%M:%S%z");
  if (formatted.size() >= 5) {
    const std::size_t offset = formatted.size() - 5;
    if ((formatted[offset] == '+' || formatted[offset] == '-') &&
        formatted[offset + 1] >= '0' && formatted[offset + 1] <= '9' &&
        formatted[offset + 2] >= '0' && formatted[offset + 2] <= '9' &&
        formatted[offset + 3] >= '0' && formatted[offset + 3] <= '9' &&
        formatted[offset + 4] >= '0' && formatted[offset + 4] <= '9') {
      formatted.insert(formatted.size() - 2, ":");
    }
  }
  return formatted;
}

bool EnsurePrivateDirectory(const std::filesystem::path& directory,
                            std::string* error) {
  std::error_code filesystem_error;
  std::filesystem::create_directories(directory, filesystem_error);
  if (filesystem_error) {
    *error =
        "cannot create session log directory: " + filesystem_error.message();
    return false;
  }
  const auto status =
      std::filesystem::symlink_status(directory, filesystem_error);
  if (filesystem_error || !std::filesystem::is_directory(status) ||
      std::filesystem::is_symlink(status)) {
    *error = "session log directory is invalid or a symlink";
    return false;
  }
  std::filesystem::permissions(directory, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace,
                               filesystem_error);
  if (filesystem_error) {
    *error = "cannot make session log directory private: " +
             filesystem_error.message();
    return false;
  }
  return true;
}

int CreateSessionFile(const std::filesystem::path& sessions,
                      std::chrono::system_clock::time_point started_at,
                      std::filesystem::path* path, std::string* error) {
  const std::string timestamp = LocalTimestamp(started_at, "%Y-%m-%d_%H-%M-%S");
  for (int collision = 1; collision <= 999; ++collision) {
    const std::string suffix =
        collision == 1 ? std::string() : "-" + std::to_string(collision);
    const std::filesystem::path candidate =
        sessions / (timestamp + suffix + ".log");
    const int descriptor =
        open(candidate.c_str(),
             O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor >= 0) {
      *path = candidate;
      return descriptor;
    }
    if (errno != EEXIST) {
      *error =
          "cannot create session log: " + std::string(std::strerror(errno));
      return -1;
    }
  }
  *error = "too many Mocktail sessions started in the same second";
  return -1;
}

bool UpdateLatestLink(const std::filesystem::path& logs_root,
                      const std::filesystem::path& session,
                      std::string* warning) {
  const std::filesystem::path latest = logs_root / "latest.log";
  const std::filesystem::path temporary =
      logs_root / (".latest.log." + std::to_string(getpid()));
  const std::filesystem::path target =
      std::filesystem::path("sessions") / session.filename();
  (void)unlink(temporary.c_str());
  if (symlink(target.c_str(), temporary.c_str()) != 0) {
    if (link(session.c_str(), temporary.c_str()) != 0) {
      *warning =
          "cannot update latest.log: " + std::string(std::strerror(errno));
      return false;
    }
  }
  if (rename(temporary.c_str(), latest.c_str()) != 0) {
    *warning =
        "cannot replace latest.log: " + std::string(std::strerror(errno));
    (void)unlink(temporary.c_str());
    return false;
  }
  return true;
}

bool WriteAll(int descriptor, const char* bytes, std::size_t size) {
  std::size_t offset = 0;
  while (offset < size) {
    const ssize_t written = write(descriptor, bytes + offset, size - offset);
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR) continue;
    return false;
  }
  return true;
}

void RunTeeProcess(int stdout_reader, int stderr_reader, int log_descriptor,
                   int original_stdout, int original_stderr) {
  (void)signal(SIGPIPE, SIG_IGN);
  (void)signal(SIGINT, SIG_IGN);
  (void)signal(SIGTERM, SIG_IGN);
  (void)signal(SIGHUP, SIG_IGN);

  struct Source {
    int reader;
    int output;
    bool active;
    bool output_active;
  };
  std::array<Source, 2> sources = {
      {{stdout_reader, original_stdout, true, true},
       {stderr_reader, original_stderr, true, true}}};
  bool log_active = true;
  std::array<char, 32768> buffer{};
  while (sources[0].active || sources[1].active) {
    std::array<pollfd, 2> descriptors = {
        {{sources[0].reader, POLLIN, 0}, {sources[1].reader, POLLIN, 0}}};
    if (!sources[0].active) descriptors[0].fd = -1;
    if (!sources[1].active) descriptors[1].fd = -1;
    const int poll_result = poll(descriptors.data(), descriptors.size(), -1);
    if (poll_result < 0) {
      if (errno == EINTR) continue;
      break;
    }
    for (std::size_t index = 0; index < sources.size(); ++index) {
      Source& source = sources[index];
      if (!source.active ||
          (descriptors[index].revents & (POLLIN | POLLHUP | POLLERR)) == 0) {
        continue;
      }
      const ssize_t bytes = read(source.reader, buffer.data(), buffer.size());
      if (bytes > 0) {
        const std::size_t size = static_cast<std::size_t>(bytes);
        if (log_active && !WriteAll(log_descriptor, buffer.data(), size)) {
          log_active = false;
        }
        if (source.output_active &&
            !WriteAll(source.output, buffer.data(), size)) {
          source.output_active = false;
        }
      } else if (bytes == 0 || errno != EINTR) {
        close(source.reader);
        source.reader = -1;
        source.active = false;
      }
    }
  }
  if (log_active) (void)fsync(log_descriptor);
  close(log_descriptor);
  close(original_stdout);
  close(original_stderr);
}

std::filesystem::path ExecutablePath() {
  std::array<char, 4097> path{};
  const ssize_t size = readlink("/proc/self/exe", path.data(), path.size() - 1);
  if (size <= 0 || static_cast<std::size_t>(size) >= path.size()) return {};
  path[static_cast<std::size_t>(size)] = '\0';
  return path.data();
}

bool IsWithin(const std::filesystem::path& child,
              const std::filesystem::path& parent) {
  const auto relative = child.lexically_relative(parent);
  return !relative.empty() && !relative.is_absolute() &&
         *relative.begin() != "..";
}

std::string DisplayPath(const std::filesystem::path& path,
                        const RuntimePaths& paths) {
  if (path.empty()) return "unknown";
  if (IsWithin(path, paths.home())) {
    return (std::filesystem::path("~") / path.lexically_relative(paths.home()))
        .string();
  }
  return path.string();
}

std::string PackageKind(const Environment& environment,
                        const std::filesystem::path& executable) {
  if (environment.HasNonEmpty("FLATPAK_ID")) return "flatpak";
  if (environment.HasNonEmpty("APPIMAGE")) return "appimage";
  if (environment.HasNonEmpty("APPDIR")) return "portable";
  const std::string path = executable.string();
  if (path.rfind("/usr/", 0) == 0 || path.rfind("/opt/", 0) == 0 ||
      path.rfind("/app/", 0) == 0) {
    return "system-install";
  }
  return "local-build";
}

std::string DisplayServer(const Environment& environment) {
  if (environment.HasNonEmpty("WAYLAND_DISPLAY")) return "wayland";
  if (environment.HasNonEmpty("DISPLAY")) return "x11";
  return "none";
}

std::string Trim(std::string value) {
  const auto first = std::find_if_not(value.begin(), value.end(), [](char c) {
    return std::isspace(static_cast<unsigned char>(c)) != 0;
  });
  const auto last = std::find_if_not(value.rbegin(), value.rend(), [](char c) {
                      return std::isspace(static_cast<unsigned char>(c)) != 0;
                    }).base();
  if (first >= last) return {};
  value = std::string(first, last);
  for (char& character : value) {
    const unsigned char byte = static_cast<unsigned char>(character);
    if (character == '"' || std::iscntrl(byte) != 0) character = ' ';
  }
  return value;
}

std::optional<std::string> ReadFirstLine(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::string value;
  if (!std::getline(input, value)) return std::nullopt;
  value = Trim(std::move(value));
  return value.empty() ? std::nullopt
                       : std::optional<std::string>(std::move(value));
}

std::optional<std::string> ReadKey(const std::filesystem::path& path,
                                   std::string_view key) {
  std::ifstream input(path);
  std::string line;
  while (std::getline(input, line)) {
    const std::size_t separator = line.find('=');
    if (separator == std::string::npos ||
        line.compare(0, separator, key) != 0 || separator != key.size()) {
      continue;
    }
    std::string value = Trim(line.substr(separator + 1));
    if (!value.empty()) return value;
  }
  return std::nullopt;
}

std::string CpuModel() {
  std::ifstream input("/proc/cpuinfo");
  std::string fallback;
  std::string line;
  while (std::getline(input, line)) {
    const std::size_t separator = line.find(':');
    if (separator == std::string::npos) continue;
    const std::string key = Trim(line.substr(0, separator));
    std::string value = Trim(line.substr(separator + 1));
    if (value.empty()) continue;
    if (key == "model name") return value;
    if (fallback.empty() && (key == "Hardware" || key == "Processor")) {
      fallback = std::move(value);
    }
  }
  if (!fallback.empty()) return fallback;
  return ReadFirstLine("/sys/devices/soc0/machine").value_or("unknown");
}

std::string TotalMemory() {
  std::ifstream input("/proc/meminfo");
  std::string key;
  std::uint64_t kibibytes = 0;
  while (input >> key) {
    if (key == "MemTotal:") {
      if (input >> kibibytes) {
        return std::to_string((kibibytes + 512U) / 1024U) + "MiB";
      }
      break;
    }
    input.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
  }
  return "unknown";
}

std::string HexId(std::optional<std::string> value) {
  if (!value.has_value()) return {};
  std::string result = Trim(std::move(*value));
  if (result.size() > 2 && result[0] == '0' &&
      (result[1] == 'x' || result[1] == 'X')) {
    result.erase(0, 2);
  }
  std::transform(result.begin(), result.end(), result.begin(), [](char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  });
  if (result.size() != 4 ||
      !std::all_of(result.begin(), result.end(), [](char c) {
        return std::isxdigit(static_cast<unsigned char>(c)) != 0;
      })) {
    return {};
  }
  return result;
}

struct PciName {
  std::string vendor;
  std::string device;
};

PciName FindPciName(std::string_view vendor_id, std::string_view device_id) {
  constexpr std::array<std::string_view, 3> kDatabases = {
      "/usr/share/hwdata/pci.ids",
      "/usr/share/misc/pci.ids",
      "/usr/share/pci.ids",
  };
  for (const std::string_view database : kDatabases) {
    std::ifstream input{std::string(database)};
    if (!input) continue;
    PciName result;
    bool matching_vendor = false;
    std::string line;
    while (std::getline(input, line)) {
      if (line.empty() || line[0] == '#') continue;
      if (line[0] != '\t') {
        if (matching_vendor) break;
        if (line.size() >= 6 && line.substr(0, 4) == vendor_id &&
            line[4] == ' ' && line[5] == ' ') {
          matching_vendor = true;
          result.vendor = Trim(line.substr(6));
        }
        continue;
      }
      if (!matching_vendor || line.size() < 7 || line[1] == '\t') continue;
      if (line.substr(1, 4) == device_id && line[5] == ' ' && line[6] == ' ') {
        result.device = Trim(line.substr(7));
        return result;
      }
    }
    if (!result.vendor.empty()) return result;
  }
  return {};
}

std::string PciVendorFallback(std::string_view vendor_id) {
  if (vendor_id == "1002") return "AMD";
  if (vendor_id == "10de") return "NVIDIA";
  if (vendor_id == "8086") return "Intel";
  if (vendor_id == "13b5") return "Arm";
  if (vendor_id == "5143") return "Qualcomm";
  if (vendor_id == "14e4") return "Broadcom";
  if (vendor_id == "1af4") return "Virtio";
  if (vendor_id == "15ad") return "VMware";
  return "PCI " + std::string(vendor_id);
}

bool DeviceNodeName(std::string_view name, std::string_view prefix) {
  if (name.size() <= prefix.size() || name.substr(0, prefix.size()) != prefix) {
    return false;
  }
  return std::all_of(
      name.begin() + static_cast<std::ptrdiff_t>(prefix.size()), name.end(),
      [](char character) {
        return std::isdigit(static_cast<unsigned char>(character)) != 0;
      });
}

struct GraphicsHardware {
  std::string devices = "unknown";
  std::string drivers = "unknown";
};

GraphicsHardware DetectGraphicsHardware() {
  std::error_code error;
  std::vector<std::filesystem::path> render_nodes;
  std::vector<std::filesystem::path> card_nodes;
  for (std::filesystem::directory_iterator iterator("/sys/class/drm", error),
       end;
       !error && iterator != end; iterator.increment(error)) {
    const std::string name = iterator->path().filename().string();
    if (DeviceNodeName(name, "renderD")) {
      render_nodes.push_back(iterator->path());
    } else if (DeviceNodeName(name, "card")) {
      card_nodes.push_back(iterator->path());
    }
  }
  std::vector<std::filesystem::path>& nodes =
      render_nodes.empty() ? card_nodes : render_nodes;
  std::sort(nodes.begin(), nodes.end());

  std::vector<std::string> seen;
  std::vector<std::string> names;
  std::vector<std::string> drivers;
  for (const auto& node : nodes) {
    error.clear();
    const std::filesystem::path device =
        std::filesystem::canonical(node / "device", error);
    if (error) continue;
    const std::string identity = device.string();
    if (std::find(seen.begin(), seen.end(), identity) != seen.end()) continue;
    seen.push_back(identity);

    error.clear();
    std::string driver = std::filesystem::read_symlink(device / "driver", error)
                             .filename()
                             .string();
    if (driver.empty()) {
      driver = ReadKey(device / "uevent", "DRIVER").value_or("unknown");
    }
    const std::string vendor_id = HexId(ReadFirstLine(device / "vendor"));
    const std::string device_id = HexId(ReadFirstLine(device / "device"));
    std::string name =
        ReadFirstLine(device / "product_name").value_or(std::string());
    if (name.empty() && !vendor_id.empty() && !device_id.empty()) {
      const PciName pci = FindPciName(vendor_id, device_id);
      const std::string vendor =
          pci.vendor.empty() ? PciVendorFallback(vendor_id) : pci.vendor;
      name = vendor +
             (pci.device.empty() ? " device " + device_id : " " + pci.device);
    }
    if (name.empty()) name = driver;
    names.push_back(Trim(std::move(name)));
    drivers.push_back(Trim(std::move(driver)));
  }

  if (names.empty()) return {};
  GraphicsHardware result;
  result.devices.clear();
  result.drivers.clear();
  for (std::size_t index = 0; index < names.size(); ++index) {
    if (index != 0) {
      result.devices += "; ";
      result.drivers += "; ";
    }
    result.devices += names[index];
    result.drivers += drivers[index];
  }
  return result;
}

}  // namespace

SessionBuildInformation CurrentSessionBuildInformation() {
  SessionBuildInformation information;
  information.version = MOCKTAIL_PROJECT_VERSION;
  information.git_commit = MOCKTAIL_BUILD_GIT_COMMIT;
  information.git_dirty = MOCKTAIL_BUILD_GIT_DIRTY;
  information.build_type = MOCKTAIL_BUILD_TYPE;
  information.compiler = std::string(MOCKTAIL_BUILD_COMPILER_ID) + " " +
                         MOCKTAIL_BUILD_COMPILER_VERSION;
  information.target = std::string(MOCKTAIL_BUILD_TARGET_SYSTEM) + "/" +
                       MOCKTAIL_BUILD_TARGET_PROCESSOR;
  return information;
}

SessionLog::~SessionLog() { Stop(); }

SessionLog::SessionLog(SessionLog&& other) noexcept {
  *this = std::move(other);
}

SessionLog& SessionLog::operator=(SessionLog&& other) noexcept {
  if (this == &other) return *this;
  Stop();
  attempted_ = other.attempted_;
  active_ = other.active_;
  original_stdout_ = other.original_stdout_;
  original_stderr_ = other.original_stderr_;
  logger_process_ = other.logger_process_;
  started_at_ = other.started_at_;
  path_ = std::move(other.path_);
  latest_path_ = std::move(other.latest_path_);
  error_ = std::move(other.error_);
  warning_ = std::move(other.warning_);
  other.attempted_ = false;
  other.active_ = false;
  other.original_stdout_ = -1;
  other.original_stderr_ = -1;
  other.logger_process_ = -1;
  return *this;
}

SessionLog SessionLog::Start(const Environment& environment,
                             const RuntimePaths& paths,
                             std::chrono::system_clock::time_point started_at) {
  SessionLog result;
  result.started_at_ = started_at;
  if (Enabled(environment, "MOCKTAIL_DISABLE_SESSION_LOG") ||
      Enabled(environment, "MOCKTAIL_ISOLATED_CANARY")) {
    return result;
  }
  result.attempted_ = true;
  std::cout.flush();
  std::cerr.flush();
  (void)fflush(nullptr);

  const std::filesystem::path sessions = paths.logs_root() / "sessions";
  if (!EnsurePrivateDirectory(paths.logs_root(), &result.error_) ||
      !EnsurePrivateDirectory(sessions, &result.error_)) {
    return result;
  }
  const int log_descriptor =
      CreateSessionFile(sessions, started_at, &result.path_, &result.error_);
  if (log_descriptor < 0) return result;
  result.latest_path_ = paths.logs_root() / "latest.log";

  std::array<int, 2> stdout_pipe = {-1, -1};
  std::array<int, 2> stderr_pipe = {-1, -1};
  result.original_stdout_ = fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
  result.original_stderr_ = fcntl(STDERR_FILENO, F_DUPFD_CLOEXEC, 3);
  if (result.original_stdout_ < 0 || result.original_stderr_ < 0 ||
      pipe2(stdout_pipe.data(), O_CLOEXEC) != 0 ||
      pipe2(stderr_pipe.data(), O_CLOEXEC) != 0) {
    result.error_ = "cannot prepare session log streams: " +
                    std::string(std::strerror(errno));
    if (stdout_pipe[0] >= 0) close(stdout_pipe[0]);
    if (stdout_pipe[1] >= 0) close(stdout_pipe[1]);
    if (stderr_pipe[0] >= 0) close(stderr_pipe[0]);
    if (stderr_pipe[1] >= 0) close(stderr_pipe[1]);
    if (result.original_stdout_ >= 0) close(result.original_stdout_);
    if (result.original_stderr_ >= 0) close(result.original_stderr_);
    result.original_stdout_ = -1;
    result.original_stderr_ = -1;
    close(log_descriptor);
    (void)unlink(result.path_.c_str());
    return result;
  }

  const pid_t child = fork();
  if (child < 0) {
    result.error_ =
        "cannot start session log writer: " + std::string(std::strerror(errno));
    close(stdout_pipe[0]);
    close(stdout_pipe[1]);
    close(stderr_pipe[0]);
    close(stderr_pipe[1]);
    close(result.original_stdout_);
    close(result.original_stderr_);
    result.original_stdout_ = -1;
    result.original_stderr_ = -1;
    close(log_descriptor);
    (void)unlink(result.path_.c_str());
    return result;
  }
  if (child == 0) {
    close(stdout_pipe[1]);
    close(stderr_pipe[1]);
    RunTeeProcess(stdout_pipe[0], stderr_pipe[0], log_descriptor,
                  result.original_stdout_, result.original_stderr_);
    _exit(0);
  }

  close(stdout_pipe[0]);
  close(stderr_pipe[0]);
  close(log_descriptor);
  int redirect_error = 0;
  const bool stdout_redirected =
      dup2(stdout_pipe[1], STDOUT_FILENO) == STDOUT_FILENO;
  if (!stdout_redirected) redirect_error = errno;
  const bool stderr_redirected =
      dup2(stderr_pipe[1], STDERR_FILENO) == STDERR_FILENO;
  if (!stderr_redirected && redirect_error == 0) redirect_error = errno;
  close(stdout_pipe[1]);
  close(stderr_pipe[1]);
  if (!stdout_redirected || !stderr_redirected) {
    if (stdout_redirected) {
      (void)dup2(result.original_stdout_, STDOUT_FILENO);
    }
    if (stderr_redirected) {
      (void)dup2(result.original_stderr_, STDERR_FILENO);
    }
    close(result.original_stdout_);
    close(result.original_stderr_);
    result.original_stdout_ = -1;
    result.original_stderr_ = -1;
    while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
    }
    result.error_ = "cannot activate session log streams: " +
                    std::string(std::strerror(redirect_error));
    (void)unlink(result.path_.c_str());
    return result;
  }
  (void)setvbuf(stdout, nullptr, _IOLBF, BUFSIZ);

  result.logger_process_ = static_cast<int>(child);
  result.active_ = true;
  (void)UpdateLatestLink(paths.logs_root(), result.path_, &result.warning_);
  return result;
}

std::string SessionLog::Header(const Environment& environment,
                               const RuntimePaths& paths,
                               std::string_view graphics_backend) const {
  const SessionBuildInformation build = CurrentSessionBuildInformation();
  const std::filesystem::path executable = ExecutablePath();
  struct utsname system = {};
  const bool has_system = uname(&system) == 0;
  const GraphicsHardware graphics = DetectGraphicsHardware();
  const unsigned int threads = std::thread::hardware_concurrency();

  std::ostringstream output;
  output << "[mocktail] version=" << build.version
         << " commit=" << build.git_commit;
  if (build.git_dirty == "true") {
    output << " dirty=true";
  } else if (build.git_dirty == "unknown") {
    output << " dirty=unknown";
  }
  output << " build=" << build.build_type << " compiler=\"" << build.compiler
         << "\"\n"
         << "[mocktail] started=" << IsoLocalTimestamp(started_at_)
         << " package=" << PackageKind(environment, executable)
         << " target=" << build.target;
  if (has_system) {
    output << " host=\"" << system.sysname << ' ' << system.release << ' '
           << system.machine << '"';
  }
  output << " pid=" << getpid() << '\n'
         << "[mocktail] cpu=\"" << CpuModel() << "\" threads=";
  if (threads == 0) {
    output << "unknown";
  } else {
    output << threads;
  }
  output << " ram=" << TotalMemory() << " gpu=\"" << graphics.devices
         << "\" gpu_driver=\"" << graphics.drivers
         << "\" display=" << DisplayServer(environment) << " graphics="
         << (graphics_backend.empty() ? "unknown" : graphics_backend) << '\n'
         << "[mocktail] executable=" << DisplayPath(executable, paths)
         << " config=" << DisplayPath(paths.config_file(), paths) << '\n'
         << "[mocktail] log=" << DisplayPath(path_, paths) << '\n';
  if (!warning_.empty()) output << "[mocktail] warning=" << warning_ << '\n';
  return output.str();
}

void SessionLog::Stop() {
  if (!active_) return;
  std::cout.flush();
  std::cerr.flush();
  (void)fflush(nullptr);
  (void)dup2(original_stdout_, STDOUT_FILENO);
  (void)dup2(original_stderr_, STDERR_FILENO);
  close(original_stdout_);
  close(original_stderr_);
  original_stdout_ = -1;
  original_stderr_ = -1;
  const pid_t child = static_cast<pid_t>(logger_process_);
  while (waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
  }
  logger_process_ = -1;
  active_ = false;
}

}  // namespace runtime
}  // namespace mocktail
