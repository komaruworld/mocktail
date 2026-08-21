#ifndef MOCKTAIL_COMPAT_BIONIC_HOST_LIBC_RUNTIME_H_
#define MOCKTAIL_COMPAT_BIONIC_HOST_LIBC_RUNTIME_H_

#include <sys/sysinfo.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/utsname.h>

#include <cstddef>
#include <cstdint>

namespace mocktail::compat {

// Android uses size_t fields here; glibc uses int and musl has no equivalent.
// An all-zero fallback means unknown, not zero usage.
struct BionicMallinfoSnapshot {
  size_t arena = 0;
  size_t ordblks = 0;
  size_t smblks = 0;
  size_t hblks = 0;
  size_t hblkhd = 0;
  size_t usmblks = 0;
  size_t fsmblks = 0;
  size_t uordblks = 0;
  size_t fordblks = 0;
  size_t keepcost = 0;
};

struct BionicLocaleState;
using BionicLocale = BionicLocaleState*;
using BionicThreadDestructor = void (*)(void*);

// Bionic uses size_t where musl uses 32-bit socklen_t.
struct BionicMessageHeader {
  void* msg_name = nullptr;
  uint32_t msg_namelen = 0;
  iovec* msg_iov = nullptr;
  size_t msg_iovlen = 0;
  void* msg_control = nullptr;
  size_t msg_controllen = 0;
  int msg_flags = 0;
};

struct BionicControlMessageHeader {
  size_t cmsg_len = 0;
  int cmsg_level = 0;
  int cmsg_type = 0;
};

// Malformed or truncated control buffers return nullptr.
BionicControlMessageHeader* BionicCmsgNextHeader(
    BionicMessageHeader* message, BionicControlMessageHeader* current) noexcept;

// Uses libstdc++'s Itanium ABI on both supported libc implementations.
int BionicCxaThreadAtExit(BionicThreadDestructor destructor, void* argument,
                          void* dso_handle) noexcept;

// Falls back to /dev/urandom; never returns partial or predictable bytes.
void BionicArc4RandomBuffer(void* buffer, size_t size) noexcept;

BionicMallinfoSnapshot BionicMallinfo() noexcept;
bool BionicMallinfoHasHostTelemetry() noexcept;

void NormalizeBionicSysInfo(struct sysinfo* info) noexcept;
int BionicSysInfo(struct sysinfo* info) noexcept;
bool NormalizeLinuxulatorUnameVersion(struct utsname* name,
                                      const char* linux_version) noexcept;
int BionicUname(struct utsname* name) noexcept;
uid_t NormalizeBionicApplicationUid(uid_t host_uid) noexcept;
uid_t BionicGetUid() noexcept;
uid_t BionicGetEffectiveUid() noexcept;

// POSIX strerror_r contract: zero on success, -1/ERANGE on truncation.
int BionicStrError(int error_number, char* buffer, size_t buffer_size) noexcept;

// Parses under host C locale without passing an incompatible Bionic locale_t.
long long BionicStrToLongLongLocale(const char* text, char** end, int base,
                                    BionicLocale locale) noexcept;
unsigned long long BionicStrToUnsignedLongLongLocale(
    const char* text, char** end, int base, BionicLocale locale) noexcept;

}  // namespace mocktail::compat

extern "C" {

mocktail::compat::BionicControlMessageHeader* mocktail_bionic_cmsg_nxthdr(
    mocktail::compat::BionicMessageHeader* message,
    mocktail::compat::BionicControlMessageHeader* current);
int mocktail_bionic_cxa_thread_atexit_impl(
    mocktail::compat::BionicThreadDestructor destructor, void* argument,
    void* dso_handle);
void mocktail_bionic_arc4random_buf(void* buffer, size_t size);
mocktail::compat::BionicMallinfoSnapshot mocktail_bionic_mallinfo();
int mocktail_bionic_sysinfo(struct sysinfo* info);
int mocktail_bionic_uname(struct utsname* name);
uid_t mocktail_bionic_getuid();
uid_t mocktail_bionic_geteuid();
int mocktail_bionic_strerror_r(int error_number, char* buffer,
                               size_t buffer_size);
long long mocktail_bionic_strtoll_l(const char* text, char** end, int base,
                                    mocktail::compat::BionicLocale locale);
unsigned long long mocktail_bionic_strtoull_l(
    const char* text, char** end, int base,
    mocktail::compat::BionicLocale locale);

}  // extern "C"

#endif  // MOCKTAIL_COMPAT_BIONIC_HOST_LIBC_RUNTIME_H_
