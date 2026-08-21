#include "compat/bionic_host_libc_runtime.h"

#include <fcntl.h>
#include <limits.h>
#include <locale.h>
#include <sys/random.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <type_traits>

#if defined(__GLIBC__)
#include <features.h>
#include <malloc.h>
#endif

namespace {

// This is the public Itanium C++ ABI entrypoint supplied by libstdc++. Unlike
// glibc's private implementation symbol, it is also available in Void's musl
// libstdc++ and owns the host-specific per-thread destructor bookkeeping.
extern "C" int __cxa_thread_atexit(void (*destructor)(void*), void* argument,
                                   void* dso_handle);

constexpr uintptr_t AlignControlMessage(size_t size) noexcept {
  constexpr uintptr_t kAlignment = sizeof(long);
  return (static_cast<uintptr_t>(size) + kAlignment - 1U) & ~(kAlignment - 1U);
}

[[noreturn]] void EntropyFailure() noexcept {
  std::fputs("mocktail: secure random source failed\n", stderr);
  std::abort();
}

bool FillFromGetRandom(unsigned char* output, size_t size) noexcept {
  size_t completed = 0;
  while (completed < size) {
    const size_t remaining = size - completed;
    const size_t request = remaining > static_cast<size_t>(SSIZE_MAX)
                               ? static_cast<size_t>(SSIZE_MAX)
                               : remaining;
    const ssize_t result = ::getrandom(output + completed, request, 0);
    if (result > 0) {
      completed += static_cast<size_t>(result);
      continue;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    if (result < 0 && (errno == ENOSYS || errno == EPERM || errno == EACCES)) {
      return false;
    }
    EntropyFailure();
  }
  return true;
}

bool ReadTextFile(const char* path, char* output, size_t capacity) noexcept {
  if (path == nullptr || output == nullptr || capacity < 2) return false;

  int descriptor;
  do {
    descriptor = ::open(path, O_RDONLY | O_CLOEXEC);
  } while (descriptor < 0 && errno == EINTR);
  if (descriptor < 0) return false;

  ssize_t length;
  do {
    length = ::read(descriptor, output, capacity - 1);
  } while (length < 0 && errno == EINTR);
  const int read_errno = errno;
  static_cast<void>(::close(descriptor));
  if (length <= 0) {
    errno = read_errno;
    return false;
  }

  size_t size = static_cast<size_t>(length);
  while (size != 0 &&
         (output[size - 1] == '\n' || output[size - 1] == '\r')) {
    --size;
  }
  output[size] = '\0';
  return size != 0;
}

void FillFromUrandom(unsigned char* output, size_t size) noexcept {
  int descriptor;
  do {
    descriptor = ::open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  } while (descriptor < 0 && errno == EINTR);
  if (descriptor < 0) {
    EntropyFailure();
  }

  size_t completed = 0;
  while (completed < size) {
    const size_t remaining = size - completed;
    const size_t request = remaining > static_cast<size_t>(SSIZE_MAX)
                               ? static_cast<size_t>(SSIZE_MAX)
                               : remaining;
    const ssize_t result = ::read(descriptor, output + completed, request);
    if (result > 0) {
      completed += static_cast<size_t>(result);
      continue;
    }
    if (result < 0 && errno == EINTR) {
      continue;
    }
    static_cast<void>(::close(descriptor));
    EntropyFailure();
  }

  if (::close(descriptor) != 0) {
    EntropyFailure();
  }
}

locale_t HostCLocale() noexcept {
  static locale_t locale = ::newlocale(LC_ALL_MASK, "C", nullptr);
  return locale;
}

template <typename Result, typename Parser>
Result ParseInHostCLocale(Parser parser) noexcept {
  const int entry_errno = errno;
  const locale_t c_locale = HostCLocale();
  if (c_locale == nullptr) {
    errno = entry_errno;
    return parser();
  }

  const locale_t previous = ::uselocale(c_locale);
  if (previous == static_cast<locale_t>(0)) {
    errno = entry_errno;
    return parser();
  }
  errno = entry_errno;
  const Result result = parser();
  const int parser_errno = errno;
  static_cast<void>(::uselocale(previous));
  errno = parser_errno;
  return result;
}

template <typename Result>
const char* HostStrErrorResult(Result result, char* buffer) noexcept {
  if constexpr (std::is_integral_v<Result>) {
    return result == 0 ? buffer : nullptr;
  } else {
    return result;
  }
}

}  // namespace

namespace mocktail::compat {

static_assert(sizeof(void*) == 8,
              "the current Bionic runtime supports Android x86-64 only");
static_assert(sizeof(BionicMessageHeader) == 56,
              "Bionic x86-64 msghdr must occupy 56 bytes");
static_assert(offsetof(BionicMessageHeader, msg_iov) == 16);
static_assert(offsetof(BionicMessageHeader, msg_controllen) == 40);
static_assert(offsetof(BionicMessageHeader, msg_flags) == 48);
static_assert(sizeof(BionicControlMessageHeader) == 16,
              "Bionic x86-64 cmsghdr must occupy 16 bytes");
static_assert(offsetof(BionicControlMessageHeader, cmsg_level) == 8);
static_assert(sizeof(BionicMallinfoSnapshot) == 10 * sizeof(size_t),
              "Bionic mallinfo must contain ten contiguous size_t fields");

BionicControlMessageHeader* BionicCmsgNextHeader(
    BionicMessageHeader* message,
    BionicControlMessageHeader* current) noexcept {
  if (message == nullptr || current == nullptr ||
      message->msg_control == nullptr) {
    return nullptr;
  }

  const uintptr_t control = reinterpret_cast<uintptr_t>(message->msg_control);
  const uintptr_t current_address = reinterpret_cast<uintptr_t>(current);
  if (message->msg_controllen >
      std::numeric_limits<uintptr_t>::max() - control) {
    return nullptr;
  }
  const uintptr_t control_end = control + message->msg_controllen;
  if (current_address < control || current_address > control_end ||
      sizeof(BionicControlMessageHeader) > control_end - current_address ||
      current->cmsg_len < sizeof(BionicControlMessageHeader)) {
    return nullptr;
  }

  const uintptr_t aligned_length = AlignControlMessage(current->cmsg_len);
  if (aligned_length < current->cmsg_len ||
      aligned_length >
          std::numeric_limits<uintptr_t>::max() - current_address) {
    return nullptr;
  }
  const uintptr_t next = current_address + aligned_length;
  if (next > control_end ||
      sizeof(BionicControlMessageHeader) > control_end - next) {
    return nullptr;
  }
  return reinterpret_cast<BionicControlMessageHeader*>(next);
}

int BionicCxaThreadAtExit(BionicThreadDestructor destructor, void* argument,
                          void* dso_handle) noexcept {
  if (destructor == nullptr) {
    return EINVAL;
  }
  return __cxa_thread_atexit(destructor, argument, dso_handle);
}

void BionicArc4RandomBuffer(void* buffer, size_t size) noexcept {
  if (size == 0) {
    return;
  }
  if (buffer == nullptr) {
    EntropyFailure();
  }

  auto* output = static_cast<unsigned char*>(buffer);
  if (!FillFromGetRandom(output, size)) {
    FillFromUrandom(output, size);
  }
}

BionicMallinfoSnapshot BionicMallinfo() noexcept {
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 33)
  const struct mallinfo2 host = ::mallinfo2();
  return {host.arena,    host.ordblks, host.smblks,  host.hblks,
          host.hblkhd,   host.usmblks, host.fsmblks, host.uordblks,
          host.fordblks, host.keepcost};
#endif
#endif
  return {};
}

bool BionicMallinfoHasHostTelemetry() noexcept {
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 33)
  return true;
#endif
#endif
  return false;
}

void NormalizeBionicSysInfo(struct sysinfo* info) noexcept {
  if (info == nullptr) return;
  constexpr unsigned long kHighBit =
      1UL << (sizeof(unsigned long) * CHAR_BIT - 1U);
  if ((info->totalswap & kHighBit) != 0 ||
      (info->freeswap & kHighBit) != 0 || info->freeswap > info->totalswap) {
    info->totalswap = 0;
    info->freeswap = 0;
  }
}

int BionicSysInfo(struct sysinfo* info) noexcept {
  const int result = ::sysinfo(info);
  if (result == 0) NormalizeBionicSysInfo(info);
  return result;
}

bool NormalizeLinuxulatorUnameVersion(struct utsname* name,
                                      const char* linux_version) noexcept {
  if (name == nullptr || linux_version == nullptr || linux_version[0] == '\0' ||
      std::strcmp(name->sysname, "Linux") != 0 ||
      (std::strstr(name->version, "FreeBSD") == nullptr &&
       std::strstr(name->version, "freebsd.org") == nullptr)) {
    return false;
  }
  const int length = std::snprintf(name->version, sizeof(name->version), "%s",
                                   linux_version);
  return length >= 0 && static_cast<size_t>(length) < sizeof(name->version);
}

int BionicUname(struct utsname* name) noexcept {
  const int result = ::uname(name);
  if (result != 0 || name == nullptr) return result;

  const int host_errno = errno;
  std::array<char, sizeof(name->version)> linux_version{};
  if (ReadTextFile("/proc/sys/kernel/version", linux_version.data(),
                   linux_version.size())) {
    NormalizeLinuxulatorUnameVersion(name, linux_version.data());
  }
  errno = host_errno;
  return result;
}

uid_t NormalizeBionicApplicationUid(uid_t host_uid) noexcept {
  return host_uid == 0 ? 1000 : host_uid;
}

uid_t BionicGetUid() noexcept {
  return NormalizeBionicApplicationUid(::getuid());
}

uid_t BionicGetEffectiveUid() noexcept {
  return NormalizeBionicApplicationUid(::geteuid());
}

int BionicStrError(int error_number, char* buffer,
                   size_t buffer_size) noexcept {
  const int entry_errno = errno;
  if (buffer == nullptr || buffer_size == 0) {
    errno = ERANGE;
    return -1;
  }

  std::array<char, 256> host_buffer{};
  auto host_result =
      ::strerror_r(error_number, host_buffer.data(), host_buffer.size());
  const char* message = HostStrErrorResult(host_result, host_buffer.data());

  std::array<char, 64> unknown_buffer{};
  if (message == nullptr) {
    std::snprintf(unknown_buffer.data(), unknown_buffer.size(),
                  "Unknown error %d", error_number);
    message = unknown_buffer.data();
  }

  const int length = std::snprintf(buffer, buffer_size, "%s", message);
  if (length < 0 || static_cast<size_t>(length) >= buffer_size) {
    errno = ERANGE;
    return -1;
  }
  errno = entry_errno;
  return 0;
}

long long BionicStrToLongLongLocale(const char* text, char** end, int base,
                                    BionicLocale locale) noexcept {
  static_cast<void>(locale);
  return ParseInHostCLocale<long long>(
      [text, end, base]() { return std::strtoll(text, end, base); });
}

unsigned long long BionicStrToUnsignedLongLongLocale(
    const char* text, char** end, int base, BionicLocale locale) noexcept {
  static_cast<void>(locale);
  return ParseInHostCLocale<unsigned long long>(
      [text, end, base]() { return std::strtoull(text, end, base); });
}

}  // namespace mocktail::compat

extern "C" mocktail::compat::BionicControlMessageHeader*
mocktail_bionic_cmsg_nxthdr(
    mocktail::compat::BionicMessageHeader* message,
    mocktail::compat::BionicControlMessageHeader* current) {
  return mocktail::compat::BionicCmsgNextHeader(message, current);
}

extern "C" int mocktail_bionic_cxa_thread_atexit_impl(
    mocktail::compat::BionicThreadDestructor destructor, void* argument,
    void* dso_handle) {
  return mocktail::compat::BionicCxaThreadAtExit(destructor, argument,
                                                 dso_handle);
}

extern "C" void mocktail_bionic_arc4random_buf(void* buffer, size_t size) {
  mocktail::compat::BionicArc4RandomBuffer(buffer, size);
}

extern "C" mocktail::compat::BionicMallinfoSnapshot mocktail_bionic_mallinfo() {
  return mocktail::compat::BionicMallinfo();
}

extern "C" int mocktail_bionic_sysinfo(struct sysinfo* info) {
  return mocktail::compat::BionicSysInfo(info);
}

extern "C" int mocktail_bionic_uname(struct utsname* name) {
  return mocktail::compat::BionicUname(name);
}

extern "C" uid_t mocktail_bionic_getuid() {
  return mocktail::compat::BionicGetUid();
}

extern "C" uid_t mocktail_bionic_geteuid() {
  return mocktail::compat::BionicGetEffectiveUid();
}

extern "C" int mocktail_bionic_strerror_r(int error_number, char* buffer,
                                          size_t buffer_size) {
  return mocktail::compat::BionicStrError(error_number, buffer, buffer_size);
}

extern "C" long long mocktail_bionic_strtoll_l(
    const char* text, char** end, int base,
    mocktail::compat::BionicLocale locale) {
  return mocktail::compat::BionicStrToLongLongLocale(text, end, base, locale);
}

extern "C" unsigned long long mocktail_bionic_strtoull_l(
    const char* text, char** end, int base,
    mocktail::compat::BionicLocale locale) {
  return mocktail::compat::BionicStrToUnsignedLongLongLocale(text, end, base,
                                                             locale);
}
