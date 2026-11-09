#ifndef MOCKTAIL_COMPAT_GUEST_ABI_H_
#define MOCKTAIL_COMPAT_GUEST_ABI_H_

#include <elf.h>
#include <string_view>

namespace mocktail::compat {

#if defined(__aarch64__)
// Android ABI name as it appears in APK lib/ directories and build metadata.
inline constexpr std::string_view kGuestAbi = "arm64-v8a";
// APK split-config name carrying the guest native libraries.
inline constexpr std::string_view kGuestApkSplitName = "config.arm64_v8a";
// File name the payload store uses for the split APK.
inline constexpr std::string_view kGuestSplitApkFile =
    "split_config.arm64_v8a.apk";
// sha256 map key holding the split APK digest in payload metadata.
inline constexpr std::string_view kGuestSplitApkHashKey = "arm64_v8a_split_apk";
// Short CPU label for device metadata and user-facing strings.
inline constexpr std::string_view kGuestCpuName = "aarch64";
inline constexpr int kGuestElfMachine = EM_AARCH64;
#elif defined(__x86_64__)
inline constexpr std::string_view kGuestAbi = "x86_64";
inline constexpr std::string_view kGuestApkSplitName = "config.x86_64";
inline constexpr std::string_view kGuestSplitApkFile = "split_config.x86_64.apk";
inline constexpr std::string_view kGuestSplitApkHashKey = "x86_64_split_apk";
inline constexpr std::string_view kGuestCpuName = "x86_64";
inline constexpr int kGuestElfMachine = EM_X86_64;
#else
#error "mocktail only supports aarch64 and x86_64 hosts"
#endif

// APK zip entry holding the guest libroblox.so.
inline constexpr std::string_view kGuestLibApkEntryPrefix = "lib/";

}  // namespace mocktail::compat

#endif  // MOCKTAIL_COMPAT_GUEST_ABI_H_
