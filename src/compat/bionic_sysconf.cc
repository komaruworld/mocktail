#include "compat/bionic_sysconf.h"

#include <errno.h>
#include <unistd.h>

namespace mocktail::compat {

long BionicSysconf(int name) noexcept {
  switch (static_cast<BionicSysconfName>(name)) {
  case BionicSysconfName::kArgMax:
    return ::sysconf(_SC_ARG_MAX);
  case BionicSysconfName::kChildMax:
    return ::sysconf(_SC_CHILD_MAX);
  case BionicSysconfName::kClockTicks:
    // Android/Linux USER_HZ is 100.
    return 100;
  case BionicSysconfName::kOpenMax:
    return ::sysconf(_SC_OPEN_MAX);
  case BionicSysconfName::kIovMax:
    return ::sysconf(_SC_IOV_MAX);
  case BionicSysconfName::kPageSize:
  case BionicSysconfName::kPageSizeAlias:
    return ::sysconf(_SC_PAGESIZE);
  case BionicSysconfName::kProcessorsConfigured:
    return ::sysconf(_SC_NPROCESSORS_CONF);
  case BionicSysconfName::kProcessorsOnline:
    return ::sysconf(_SC_NPROCESSORS_ONLN);
  case BionicSysconfName::kPhysicalPages:
    return ::sysconf(_SC_PHYS_PAGES);
  case BionicSysconfName::kAvailablePhysicalPages:
    return ::sysconf(_SC_AVPHYS_PAGES);
  }

  errno = EINVAL;
  return -1;
}

} // namespace mocktail::compat

extern "C" long mocktail_bionic_sysconf(int name) {
  return mocktail::compat::BionicSysconf(name);
}
