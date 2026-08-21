#include "compat/bionic_prctl_runtime.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/prctl.h>
#include <unistd.h>

namespace mocktail::compat {
namespace {

#ifndef PR_SET_VMA
constexpr int kPrSetVma = 0x53564d41;
#else
constexpr int kPrSetVma = PR_SET_VMA;
#endif

#ifndef PR_SET_VMA_ANON_NAME
constexpr unsigned long kPrSetVmaAnonName = 0;
#else
constexpr unsigned long kPrSetVmaAnonName = PR_SET_VMA_ANON_NAME;
#endif

#ifndef PR_SET_THP_DISABLE
constexpr int kPrSetThpDisable = 41;
#else
constexpr int kPrSetThpDisable = PR_SET_THP_DISABLE;
#endif

#ifndef PR_GET_THP_DISABLE
constexpr int kPrGetThpDisable = 42;
#else
constexpr int kPrGetThpDisable = PR_GET_THP_DISABLE;
#endif

bool IsUnsupportedPrctlError(int error) noexcept {
  return error == EINVAL || error == ENOSYS || error == EOPNOTSUPP;
}

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

bool IsPageAlignedRange(unsigned long address, unsigned long length) noexcept {
  if (address == 0 || length == 0 || address + length < address) return false;
  const long page_size = ::sysconf(_SC_PAGESIZE);
  if (page_size <= 0) return false;
  const auto page_mask = static_cast<unsigned long>(page_size - 1);
  return (address & page_mask) == 0 && (length & page_mask) == 0;
}

}  // namespace

int BionicPrctl(int option, unsigned long argument2,
                unsigned long argument3, unsigned long argument4,
                unsigned long argument5) noexcept {
  const int previous_errno = errno;
  const int result = ::prctl(option, argument2, argument3, argument4,
                             argument5);
  if (result != -1 || !IsFreeBsdLinuxulator() ||
      !IsUnsupportedPrctlError(errno)) {
    return result;
  }

  if (option == kPrGetThpDisable) {
    errno = previous_errno;
    return 1;
  }
  if (option == kPrSetThpDisable && argument2 != 0) {
    errno = previous_errno;
    return 0;
  }

  if (option == kPrSetVma && argument2 == kPrSetVmaAnonName &&
      IsPageAlignedRange(argument3, argument4)) {
    errno = previous_errno;
    return 0;
  }

  return result;
}

}  // namespace mocktail::compat

extern "C" int mocktail_bionic_prctl(int option, unsigned long argument2,
                                      unsigned long argument3,
                                      unsigned long argument4,
                                      unsigned long argument5) {
  return mocktail::compat::BionicPrctl(option, argument2, argument3,
                                       argument4, argument5);
}
