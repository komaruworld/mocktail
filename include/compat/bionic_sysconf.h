#ifndef MOCKTAIL_COMPAT_BIONIC_SYSCONF_H_
#define MOCKTAIL_COMPAT_BIONIC_SYSCONF_H_

namespace mocktail::compat {

// Android Bionic assigns different integer values to sysconf names than
// glibc. These are the names observed in the active Roblox Build-ID profile
// and are kept explicit so a raw Bionic value can never reach host sysconf.
enum class BionicSysconfName : int {
  kArgMax = 0x0000,
  kChildMax = 0x0005,
  kClockTicks = 0x0006,
  kOpenMax = 0x000b,
  kIovMax = 0x0026,
  kPageSize = 0x0027,
  kPageSizeAlias = 0x0028,
  kProcessorsConfigured = 0x0060,
  kProcessorsOnline = 0x0061,
  kPhysicalPages = 0x0062,
  kAvailablePhysicalPages = 0x0063,
};

// Translates selected Bionic sysconf integer names to their host glibc
// equivalent. Unsupported names fail exactly as Bionic does: -1 and EINVAL.
long BionicSysconf(int name) noexcept;

}  // namespace mocktail::compat

extern "C" long mocktail_bionic_sysconf(int name);

#endif  // MOCKTAIL_COMPAT_BIONIC_SYSCONF_H_
