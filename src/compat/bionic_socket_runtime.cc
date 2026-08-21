#include "compat/bionic_socket_runtime.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <spawn.h>
#include <string>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

#ifndef MOCKTAIL_INSTALL_LIBDIR
#define MOCKTAIL_INSTALL_LIBDIR "lib"
#endif

namespace mocktail::compat {
namespace {

constexpr const char kFreeBsdSocketHelper[] =
    "mocktail_freebsd_socket_helper";

#ifndef SOL_UDP
constexpr int kSolUdp = 17;
#else
constexpr int kSolUdp = SOL_UDP;
#endif

#ifndef UDP_SEGMENT
constexpr int kUdpSegment = 103;
#else
constexpr int kUdpSegment = UDP_SEGMENT;
#endif

#ifndef UDP_GRO
constexpr int kUdpGro = 104;
#else
constexpr int kUdpGro = UDP_GRO;
#endif

bool IsFreeBsdLinuxulator() noexcept {
  const int saved_errno = errno;
  static const bool detected = [] {
    int descriptor = ::open("/proc/version", O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) return false;
    char version[512] = {};
    const ssize_t length = ::read(descriptor, version, sizeof(version) - 1);
    ::close(descriptor);
    if (length <= 0) return false;
    version[length] = '\0';
    return std::strstr(version, "FreeBSD") != nullptr ||
           std::strstr(version, "freebsd.org") != nullptr;
  }();
  errno = saved_errno;
  return detected;
}

bool IsUnsupportedSocketOptionError(int error) noexcept {
  return error == ENOPROTOOPT || error == EINVAL || error == EOPNOTSUPP;
}

struct UdpSegmentControl {
  bool present = false;
  uint16_t segment_size = 0;
};

struct ParsedControlMessages {
  UdpSegmentControl udp_segment;
  std::vector<unsigned char> passthrough;
};

bool ParseControlMessages(const msghdr *message,
                          ParsedControlMessages *result) {
  if (message == nullptr || result == nullptr) return false;
  if (message->msg_controllen == 0) return true;
  if (message->msg_control == nullptr) return false;

  const auto *cursor =
      static_cast<const unsigned char *>(message->msg_control);
  std::size_t remaining = message->msg_controllen;
  bool udp_segment_seen = false;
  while (remaining >= sizeof(cmsghdr)) {
    cmsghdr header{};
    std::memcpy(&header, cursor, sizeof(header));
    const std::size_t length = header.cmsg_len;
    if (length < sizeof(cmsghdr) || length > remaining ||
        length > SIZE_MAX - (sizeof(size_t) - 1U)) {
      return false;
    }
    const std::size_t aligned_length = CMSG_ALIGN(length);
    if (aligned_length < length) return false;

    const bool udp_segment =
        header.cmsg_level == kSolUdp && header.cmsg_type == kUdpSegment;
    if (udp_segment) {
      if (length < CMSG_LEN(sizeof(uint16_t))) return false;
      if (!udp_segment_seen) {
        std::memcpy(&result->udp_segment.segment_size,
                    cursor + CMSG_ALIGN(sizeof(cmsghdr)), sizeof(uint16_t));
        result->udp_segment.present =
            result->udp_segment.segment_size != 0;
        udp_segment_seen = true;
      }
    } else {
      const std::size_t old_size = result->passthrough.size();
      if (aligned_length > SIZE_MAX - old_size ||
          aligned_length > result->passthrough.max_size() - old_size) {
        return false;
      }
      result->passthrough.resize(old_size + aligned_length, 0);
      std::memcpy(result->passthrough.data() + old_size, cursor, length);
    }

    const std::size_t consumed = std::min(aligned_length, remaining);
    cursor += consumed;
    remaining -= consumed;
  }
  return true;
}

bool IovecByteCount(const msghdr *message, std::size_t *result) noexcept {
  if (message == nullptr || result == nullptr ||
      message->msg_iovlen > static_cast<std::size_t>(IOV_MAX) ||
      (message->msg_iovlen != 0 && message->msg_iov == nullptr)) {
    return false;
  }
  std::size_t total = 0;
  for (std::size_t index = 0; index < message->msg_iovlen; ++index) {
    const std::size_t length = message->msg_iov[index].iov_len;
    if (length > SIZE_MAX - total) return false;
    total += length;
  }
  *result = total;
  return true;
}

std::vector<iovec> SliceIovecs(const msghdr *message, std::size_t offset,
                               std::size_t length) {
  std::vector<iovec> result;
  for (std::size_t index = 0;
       index < message->msg_iovlen && length != 0; ++index) {
    const iovec &source = message->msg_iov[index];
    if (offset >= source.iov_len) {
      offset -= source.iov_len;
      continue;
    }
    const std::size_t available = source.iov_len - offset;
    const std::size_t selected = available < length ? available : length;
    auto *base = static_cast<unsigned char *>(source.iov_base) + offset;
    result.push_back({base, selected});
    length -= selected;
    offset = 0;
  }
  return result;
}

bool DiscoveryModeUsesDontFragment(int mode, bool *enabled) noexcept {
  if (enabled == nullptr) return false;
  switch (mode) {
    case IP_PMTUDISC_DONT:
      *enabled = false;
      return true;
    case IP_PMTUDISC_WANT:
    case IP_PMTUDISC_DO:
    case IP_PMTUDISC_PROBE:
      *enabled = true;
      return true;
#ifdef IP_PMTUDISC_INTERFACE
    case IP_PMTUDISC_INTERFACE:
      *enabled = false;
      return true;
#endif
#ifdef IP_PMTUDISC_OMIT
    case IP_PMTUDISC_OMIT:
      *enabled = false;
      return true;
#endif
    default:
      return false;
  }
}

std::string ResolveFreeBsdSocketHelperPath(const std::string &executable_path,
                                           const char *override_path) {
  if (override_path != nullptr && override_path[0] == '/') {
    return override_path;
  }

  const std::size_t separator = executable_path.find_last_of('/');
  if (separator == std::string::npos) return {};
  const std::string executable_directory =
      executable_path.substr(0, separator + 1);
  const std::string adjacent = executable_directory + kFreeBsdSocketHelper;
  if (::access(adjacent.c_str(), X_OK) == 0) return adjacent;

  constexpr const char kInstallLibDir[] = MOCKTAIL_INSTALL_LIBDIR;
  const std::string installed =
      kInstallLibDir[0] == '/'
          ? std::string(kInstallLibDir) + "/mocktail/" +
                kFreeBsdSocketHelper
          : executable_directory + "../" + kInstallLibDir + "/mocktail/" +
                kFreeBsdSocketHelper;
  if (::access(installed.c_str(), X_OK) == 0) return installed;
  return {};
}

std::string FreeBsdSocketHelperPath() {
  std::array<char, PATH_MAX + 1> executable{};
  const ssize_t length =
      ::readlink("/proc/self/exe", executable.data(), PATH_MAX);
  if (length <= 0 || length > PATH_MAX) return {};
  executable[static_cast<std::size_t>(length)] = '\0';
  return ResolveFreeBsdSocketHelperPath(
      std::string(executable.data(), static_cast<std::size_t>(length)),
      std::getenv("MOCKTAIL_FREEBSD_SOCKET_HELPER"));
}

int ApplyFreeBsdPathMtuDiscovery(int socket, int discovery_mode) noexcept {
  bool dont_fragment = false;
  if (!DiscoveryModeUsesDontFragment(discovery_mode, &dont_fragment)) {
    return -1;
  }

  const std::string helper = FreeBsdSocketHelperPath();
  if (helper.empty() || ::access(helper.c_str(), X_OK) != 0) return -1;

  const int inherited_socket = ::fcntl(socket, F_DUPFD, 3);
  if (inherited_socket < 0) return -1;

  std::array<char, 32> descriptor{};
  const int descriptor_length = std::snprintf(
      descriptor.data(), descriptor.size(), "%d", inherited_socket);
  if (descriptor_length <= 0 ||
      static_cast<std::size_t>(descriptor_length) >= descriptor.size()) {
    ::close(inherited_socket);
    return -1;
  }

  char enabled[] = "0";
  if (dont_fragment) enabled[0] = '1';
  char *arguments[] = {
      const_cast<char *>(kFreeBsdSocketHelper), descriptor.data(), enabled,
      nullptr};
  pid_t child = -1;
  const int spawn_status =
      ::posix_spawn(&child, helper.c_str(), nullptr, nullptr, arguments,
                    environ);
  ::close(inherited_socket);
  if (spawn_status != 0) return -1;

  int child_status = 0;
  pid_t waited = -1;
  do {
    waited = ::waitpid(child, &child_status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited != child || !WIFEXITED(child_status) ||
      WEXITSTATUS(child_status) != 0) {
    return -1;
  }

  return 0;
}

}  // namespace

int BionicSetsockopt(int socket, int level, int option_name,
                     const void *option_value,
                     socklen_t option_length) noexcept {
  const int previous_errno = errno;
  const int result =
      ::setsockopt(socket, level, option_name, option_value, option_length);
  if (result == -1 && errno == ENOPROTOOPT && level == IPPROTO_IP &&
      option_name == IP_MTU_DISCOVER && option_value != nullptr &&
      option_length == sizeof(int)) {
    const int discovery_mode = *static_cast<const int *>(option_value);
    if (ApplyFreeBsdPathMtuDiscovery(socket, discovery_mode) == 0) {
      errno = previous_errno;
      return 0;
    }
    bool dont_fragment = false;
    if (IsFreeBsdLinuxulator() &&
        DiscoveryModeUsesDontFragment(discovery_mode, &dont_fragment)) {
      errno = previous_errno;
      return 0;
    }
    errno = ENOPROTOOPT;
  }
  if (result == -1 && IsFreeBsdLinuxulator() &&
      IsUnsupportedSocketOptionError(errno) && level == kSolUdp &&
      option_name == kUdpGro && option_value != nullptr &&
      option_length == sizeof(int)) {
    const int enabled = *static_cast<const int *>(option_value);
    if (enabled == 0 || enabled == 1) {
      errno = previous_errno;
      return 0;
    }
  }
  return result;
}

ssize_t BionicSendmsg(int socket, const msghdr *message, int flags) noexcept {
  if (message == nullptr) {
    errno = EINVAL;
    return -1;
  }

  const int previous_errno = errno;
  const ssize_t result = ::sendmsg(socket, message, flags);
  if (result != -1 || !IsFreeBsdLinuxulator() ||
      !IsUnsupportedSocketOptionError(errno)) {
    return result;
  }

  ParsedControlMessages control;
  if (!ParseControlMessages(message, &control)) {
    errno = EINVAL;
    return -1;
  }
  if (!control.udp_segment.present) return result;
  std::size_t total = 0;
  if (!IovecByteCount(message, &total)) {
    errno = EINVAL;
    return -1;
  }
  if (total == 0) return result;

  std::size_t sent_total = 0;
  while (sent_total < total) {
    const std::size_t segment_length =
        std::min<std::size_t>(control.udp_segment.segment_size,
                              total - sent_total);
    std::vector<iovec> iovecs =
        SliceIovecs(message, sent_total, segment_length);
    if (iovecs.empty()) {
      errno = EINVAL;
      return -1;
    }

    msghdr segment = *message;
    segment.msg_iov = iovecs.data();
    segment.msg_iovlen = iovecs.size();
    segment.msg_control =
        control.passthrough.empty() ? nullptr : control.passthrough.data();
    segment.msg_controllen = control.passthrough.size();
    const ssize_t sent = ::sendmsg(socket, &segment, flags);
    if (sent < 0) return -1;
    if (static_cast<std::size_t>(sent) != segment_length) {
      errno = EIO;
      return -1;
    }
    sent_total += segment_length;
  }

  errno = previous_errno;
  return static_cast<ssize_t>(sent_total);
}

}  // namespace mocktail::compat

extern "C" int mocktail_bionic_setsockopt(int socket, int level,
                                          int option_name,
                                          const void *option_value,
                                          socklen_t option_length) {
  return mocktail::compat::BionicSetsockopt(socket, level, option_name,
                                            option_value, option_length);
}

extern "C" ssize_t mocktail_bionic_sendmsg(int socket,
                                            const struct msghdr *message,
                                            int flags) {
  return mocktail::compat::BionicSendmsg(socket, message, flags);
}
