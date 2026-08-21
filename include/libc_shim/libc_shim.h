#ifndef MOCKTAIL_LIBC_SHIM_LIBC_SHIM_H_
#define MOCKTAIL_LIBC_SHIM_LIBC_SHIM_H_

#include <cstdio>
#include <dirent.h>
#include <string>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/types.h>

namespace libc_shim {

using GuestAllocator = void* (*)(size_t);

enum class HostCaBundleStatus {
  kReady,
  kInvalidOverride,
  kUnavailable,
};

struct HostCaBundleResolution {
  HostCaBundleStatus status = HostCaBundleStatus::kUnavailable;
  std::string host_path;
  bool from_override = false;

  bool ok() const { return status == HostCaBundleStatus::kReady; }
};

// Selects the allocator used by libc APIs whose POSIX contract returns an
// owned buffer. This keeps allocations on the same side of the host/guest ABI
// boundary as the caller that will later free them.
void ConfigureGuestAllocator(GuestAllocator allocator) noexcept;

// Must run before loading the target library so its Bionic imports resolve to
// these shims.
void Install();

// Resolves the host TLS trust bundle without weakening certificate
// verification. MOCKTAIL_CA_BUNDLE, when present, is authoritative and must
// name a readable, non-empty, absolute regular file; an invalid override never
// falls back to a system path.
HostCaBundleResolution ResolveHostCaBundle();

// Resolves and registers exact Android CA-file aliases. The aliases are more
// specific than the application data prefix and therefore retain the existing
// longest-prefix path semantics for every other application path.
HostCaBundleResolution ConfigureHostCaBundlePathMappings();

const char* HostCaBundleStatusName(HostCaBundleStatus status);

// Applies the longest matching Android-to-host path prefix. Unmapped paths are
// returned unchanged.
std::string TranslatePath(const std::string& android_path);

void RegisterPathMapping(const std::string& android_prefix,
                         const std::string& host_prefix);

void ClearPathMappings();

}  // namespace libc_shim

extern "C" {
int mocktail_open(const char* path, int flags, ...);
int mocktail___open_2(const char* path, int flags);
FILE* mocktail_fopen(const char* path, const char* mode);
int mocktail_access(const char* path, int mode);
int mocktail_stat(const char* path, struct stat* statbuf);
int mocktail_lstat(const char* path, struct stat* statbuf);
int mocktail_statvfs(const char* path, struct statvfs* statbuf);
int mocktail_statfs(const char* path, struct statfs* statbuf);
int mocktail_mkdir(const char* path, mode_t mode);
DIR* mocktail_opendir(const char* path);
int mocktail_rename(const char* old_path, const char* new_path);
int mocktail_unlink(const char* path);
int mocktail_rmdir(const char* path);
char* mocktail_realpath(const char* path, char* resolved_path);
ssize_t mocktail_readlink(const char* path, char* buf, size_t bufsiz);
ssize_t mocktail___readlink_chk(const char* path, char* buf, size_t len,
                                size_t buf_len);
}

#endif  // MOCKTAIL_LIBC_SHIM_LIBC_SHIM_H_
