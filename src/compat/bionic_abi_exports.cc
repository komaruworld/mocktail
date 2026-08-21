#include "compat/bionic_abi_exports.h"

#include <poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

#include "compat/http_client_spin_guard.h"
#include "window/window.h"

volatile uintptr_t g_mocktail_abort_libroblox_base = 0;
std::atomic<void*> g_current_jni_env_for_publish{nullptr};

namespace {

std::atomic<bool> g_legacy_bionic_diagnostics_enabled{false};

const char* EnvironmentOr(const char* name, const char* fallback) {
  const char* value = std::getenv(name);
  return value != nullptr && value[0] != '\0' ? value : fallback;
}

int CallHostVsnprintf(char* dst, size_t count, const char* format,
                      va_list args) {
  using HostVsnprintf = int (*)(char*, size_t, const char*, va_list);
  HostVsnprintf volatile host_vsnprintf = &std::vsnprintf;
  return host_vsnprintf(dst, count, format, args);
}

ssize_t CallHostRead(int fd, void* buf, size_t count) {
  using HostRead = ssize_t (*)(int, void*, size_t);
  HostRead volatile host_read = &::read;
  return host_read(fd, buf, count);
}

ssize_t CallHostWrite(int fd, const void* buf, size_t count) {
  using HostWrite = ssize_t (*)(int, const void*, size_t);
  HostWrite volatile host_write = &::write;
  return host_write(fd, buf, count);
}

ssize_t CallHostPread(int fd, void* buf, size_t count, off_t offset) {
  using HostPread = ssize_t (*)(int, void*, size_t, off_t);
  HostPread volatile host_pread = &::pread;
  return host_pread(fd, buf, count, offset);
}

int CallHostPoll(struct pollfd* fds, nfds_t nfds, int timeout) {
  using HostPoll = int (*)(struct pollfd*, nfds_t, int);
  HostPoll volatile host_poll = &::poll;
  return host_poll(fds, nfds, timeout);
}

ssize_t CallHostSendto(int socket_fd, const void* buf, size_t len, int flags,
                       const struct sockaddr* dest, socklen_t dest_len) {
  using HostSendto = ssize_t (*)(int, const void*, size_t, int,
                                 const struct sockaddr*, socklen_t);
  HostSendto volatile host_sendto = &::sendto;
  return host_sendto(socket_fd, buf, len, flags, dest, dest_len);
}

}  // namespace

namespace mocktail::compat {

void SetLegacyBionicDiagnosticsEnabled(bool enabled) {
  g_legacy_bionic_diagnostics_enabled.store(enabled, std::memory_order_release);
}

}  // namespace mocktail::compat

// EGL handle exports — resolved by libegl_stub via dlsym(RTLD_DEFAULT, sym).
// These bridge the window.cc real EGL context into the stub library.
extern "C" {
void* mocktail_egl_display() { return mocktail::window::GetEGLDisplay(); }
void* mocktail_egl_surface() { return mocktail::window::GetEGLSurface(); }
void* mocktail_egl_context() { return mocktail::window::GetEGLContext(); }
void* mocktail_egl_config() { return mocktail::window::GetEGLConfig(); }
void* mocktail_native_window() { return mocktail::window::GetNativeWindow(); }
__attribute__((visibility("default"))) void mocktail_set_current_jni_env(
    void* env) {
  g_current_jni_env_for_publish.store(env, std::memory_order_release);
}
__attribute__((visibility("default"))) void* mocktail_get_current_jni_env() {
  return g_current_jni_env_for_publish.load(std::memory_order_acquire);
}
__attribute__((visibility("default"))) uintptr_t __stack_chk_guard =
    0x595e9fbd94fda766ULL;
int* __errno() { return &errno; }
void __assert(const char* file, int line, const char* failed_expression) {
  std::fprintf(stderr, "Bionic assertion failed: %s:%d: %s\n",
               file ? file : "(unknown)", line,
               failed_expression ? failed_expression : "(unknown)");
  std::abort();
}
void __assert2(const char* file, int line, const char* function,
               const char* failed_expression) {
  std::fprintf(stderr, "Bionic assertion failed: %s:%d: %s: %s\n",
               file ? file : "(unknown)", line,
               function ? function : "(unknown)",
               failed_expression ? failed_expression : "(unknown)");
  std::abort();
}
__attribute__((noreturn)) void mocktail_abort() {
  uintptr_t caller = reinterpret_cast<uintptr_t>(__builtin_return_address(0));
  uintptr_t rax = 0;
  uintptr_t rbx = 0;
  uintptr_t rdi = 0;
  uintptr_t r15 = 0;
#if defined(__x86_64__)
  asm volatile("mov %%rax,%0" : "=r"(rax));
  asm volatile("mov %%rbx,%0" : "=r"(rbx));
  asm volatile("mov %%rdi,%0" : "=r"(rdi));
  asm volatile("mov %%r15,%0" : "=r"(r15));
#endif
  uintptr_t base = static_cast<uintptr_t>(g_mocktail_abort_libroblox_base);
  uintptr_t caller_offset = (base != 0 && caller >= base) ? caller - base : 0;
  std::fprintf(stderr,
               "  [abort] bionic abort caller=%p off=0x%lx "
               "rax=%p rbx=%p rdi=%p r15=%p\n",
               reinterpret_cast<void*>(caller),
               static_cast<unsigned long>(caller_offset),
               reinterpret_cast<void*>(rax), reinterpret_cast<void*>(rbx),
               reinterpret_cast<void*>(rdi), reinterpret_cast<void*>(r15));
  if (g_legacy_bionic_diagnostics_enabled.load(std::memory_order_acquire) &&
      caller_offset == 0x2c18f35 && rbx >= 0x10000 &&
      rbx < 0x0000800000000000ULL) {
    const auto* key = reinterpret_cast<const uint64_t*>(rbx);
    std::fprintf(stderr,
                 "  [abort] emutls key=%p size=0x%llx align=0x%llx "
                 "index=0x%llx init=0x%llx\n",
                 reinterpret_cast<const void*>(key),
                 static_cast<unsigned long long>(key[0]),
                 static_cast<unsigned long long>(key[1]),
                 static_cast<unsigned long long>(key[2]),
                 static_cast<unsigned long long>(key[3]));
  }
  std::fflush(stderr);
  std::abort();
}
char* __gnu_strerror_r(int error_number, char* buf, size_t buf_len) {
  if (buf == nullptr || buf_len == 0) {
    errno = ERANGE;
    return buf;
  }
  const char* message = std::strerror(error_number);
  if (message == nullptr) {
    std::snprintf(buf, buf_len, "Unknown error %d", error_number);
  } else {
    std::snprintf(buf, buf_len, "%s", message);
  }
  return buf;
}
int __system_property_get(const char* name, char* value) {
  if (value == nullptr) {
    return 0;
  }
  const char* result = "";
  if (name == nullptr) {
    result = "";
  } else if (std::strcmp(name, "ro.build.version.sdk") == 0) {
    result = "35";
  } else if (std::strcmp(name, "ro.build.version.release") == 0) {
    result = "15";
  } else if (std::strcmp(name, "ro.product.cpu.abi") == 0) {
    result = "x86_64";
  } else if (std::strcmp(name, "ro.product.manufacturer") == 0) {
    result = EnvironmentOr("MOCKTAIL_DEVICE_MANUFACTURER", "Mocktail");
  } else if (std::strcmp(name, "ro.product.model") == 0) {
    result = EnvironmentOr("MOCKTAIL_DEVICE_MODEL", "Mocktail Linux");
  } else if (std::strcmp(name, "ro.product.brand") == 0) {
    result = EnvironmentOr("MOCKTAIL_DEVICE_BRAND", "google");
  } else if (std::strcmp(name, "ro.product.device") == 0) {
    result = EnvironmentOr("MOCKTAIL_DEVICE_CODE", "mocktail");
  } else if (std::strcmp(name, "ro.hardware") == 0) {
    result = "ranchu";
  } else if (std::strcmp(name, "debug.hwui.renderer") == 0) {
    result = "skiagl";
  }
  constexpr size_t kAndroidPropertyValueMax = 92;
  std::strncpy(value, result, kAndroidPropertyValueMax - 1);
  value[kAndroidPropertyValueMax - 1] = '\0';
  return static_cast<int>(std::strlen(value));
}
void* __memcpy_chk(void* dst, const void* src, size_t count, size_t dst_len) {
  if (count > dst_len) {
    errno = ERANGE;
    count = dst_len;
  }
  return std::memcpy(dst, src, count);
}
void* __memmove_chk(void* dst, const void* src, size_t count, size_t dst_len) {
  if (count > dst_len) {
    errno = ERANGE;
    count = dst_len;
  }
  return std::memmove(dst, src, count);
}
void* __memset_chk(void* dst, int value, size_t count, size_t dst_len) {
  if (count > dst_len) {
    errno = ERANGE;
    count = dst_len;
  }
  return std::memset(dst, value, count);
}
size_t __strlen_chk(const char* value, size_t /*value_len*/) {
  return std::strlen(value);
}
char* __strchr_chk(const char* value, int ch, size_t /*value_len*/) {
  return const_cast<char*>(std::strchr(value, ch));
}
char* __strcpy_chk(char* dst, const char* src, size_t dst_len) {
  size_t src_len = std::strlen(src) + 1;
  if (src_len > dst_len) {
    errno = ERANGE;
    src_len = dst_len;
  }
  if (src_len > 0) {
    std::memcpy(dst, src, src_len);
    dst[src_len - 1] = '\0';
  }
  return dst;
}
char* __strncpy_chk(char* dst, const char* src, size_t count, size_t dst_len) {
  if (count > dst_len) {
    errno = ERANGE;
    count = dst_len;
  }
  return std::strncpy(dst, src, count);
}
char* __strncpy_chk2(char* dst, const char* src, size_t count, size_t dst_len,
                     size_t src_len) {
  size_t bounded_count = count;
  if (bounded_count > dst_len) {
    errno = ERANGE;
    bounded_count = dst_len;
  }
  if (bounded_count > src_len) {
    errno = ERANGE;
    bounded_count = src_len;
  }
  return std::strncpy(dst, src, bounded_count);
}
char* __strcat_chk(char* dst, const char* src, size_t dst_len) {
  size_t dst_used = std::strlen(dst);
  if (dst_used >= dst_len) {
    errno = ERANGE;
    return dst;
  }
  size_t remaining = dst_len - dst_used - 1;
  std::strncat(dst, src, remaining);
  return dst;
}
int __vsprintf_chk(char* dst, int /*flags*/, size_t dst_len, const char* format,
                   va_list args) {
  return CallHostVsnprintf(dst, dst_len, format, args);
}
int __vsnprintf_chk(char* dst, size_t count, int /*flags*/, size_t dst_len,
                    const char* format, va_list args) {
  size_t bounded_count = std::min(count, dst_len);
  return CallHostVsnprintf(dst, bounded_count, format, args);
}
ssize_t __read_chk(int fd, void* buf, size_t count, size_t buf_len) {
  if (count > buf_len) {
    errno = ERANGE;
    count = buf_len;
  }
  return CallHostRead(fd, buf, count);
}
ssize_t __write_chk(int fd, const void* buf, size_t count, size_t buf_len) {
  if (count > buf_len) {
    errno = ERANGE;
    count = buf_len;
  }
  return CallHostWrite(fd, buf, count);
}
ssize_t __pread64_chk(int fd, void* buf, size_t count, off_t offset,
                      size_t buf_len) {
  if (count > buf_len) {
    errno = ERANGE;
    count = buf_len;
  }
  return CallHostPread(fd, buf, count, offset);
}
bool mocktail_is_low_bionic_pointer(const void* ptr) {
  return reinterpret_cast<uintptr_t>(ptr) < 4096;
}

struct MocktailHostMutexEntry {
  uintptr_t guest_address = 0;
  pthread_mutex_t host_mutex{};
  int bionic_type = 0;
  std::atomic<uint64_t> held_depth{0};
  MocktailHostMutexEntry* next = nullptr;
};

struct MocktailHostCondEntry {
  uintptr_t guest_address;
  pthread_cond_t host_cond;
  MocktailHostCondEntry* next;
};

constexpr size_t kHostSyncBucketCount = 4096;
constexpr size_t kHostSyncShardCount = 64;
constexpr size_t kHostMutexThreadCacheCount = 64;
// Android x86-64 stores pthread_mutex_t as ten int32_t values. Its first
// uint16_t carries the mutex type in bits 15:14, while pthread_mutexattr_t is
// an eight-byte long whose low nibble carries the same type. These layouts are
// not compatible with the host libc definitions used by this translation
// unit, so read and write them as guest bytes.
constexpr size_t kBionicMutexSize = 40;
constexpr uint16_t kBionicMutexDestroyedState = 0xffff;
constexpr uint16_t kBionicMutexTypeShift = 14;
constexpr uint16_t kBionicMutexTypeMask = 0x3;
constexpr int64_t kBionicMutexAttrTypeMask = 0x0f;
constexpr int kBionicMutexNormal = 0;
constexpr int kBionicMutexRecursive = 1;
constexpr int kBionicMutexErrorCheck = 2;
static_assert((kHostSyncBucketCount & (kHostSyncBucketCount - 1)) == 0);
static_assert((kHostSyncShardCount & (kHostSyncShardCount - 1)) == 0);
static_assert((kHostMutexThreadCacheCount &
               (kHostMutexThreadCacheCount - 1)) == 0);
static_assert(sizeof(pthread_mutex_t) >= kBionicMutexSize);

struct MocktailHostMutexCacheEntry {
  uintptr_t guest_address = 0;
  MocktailHostMutexEntry* host_entry = nullptr;
  uint64_t epoch = 0;
};

MocktailHostMutexEntry* g_mocktail_host_mutex_buckets[kHostSyncBucketCount]{};
MocktailHostCondEntry* g_mocktail_host_cond_buckets[kHostSyncBucketCount]{};
pthread_mutex_t g_mocktail_host_mutex_shards[kHostSyncShardCount];
pthread_mutex_t g_mocktail_host_cond_shards[kHostSyncShardCount];
pthread_once_t g_mocktail_host_sync_once = PTHREAD_ONCE_INIT;
std::atomic<int> g_mocktail_host_sync_init_result{0};
std::atomic<bool> g_mocktail_host_sync_ready{false};
std::atomic<uint64_t> g_mocktail_host_mutex_cache_epoch{1};
thread_local std::array<MocktailHostMutexCacheEntry,
                        kHostMutexThreadCacheCount>
    g_mocktail_host_mutex_cache;

void mocktail_initialize_host_sync_tables() {
  for (size_t i = 0; i < kHostSyncShardCount; ++i) {
    int result =
        ::pthread_mutex_init(&g_mocktail_host_mutex_shards[i], nullptr);
    if (result == 0) {
      result = ::pthread_mutex_init(&g_mocktail_host_cond_shards[i], nullptr);
    }
    if (result != 0) {
      g_mocktail_host_sync_init_result.store(result, std::memory_order_release);
      return;
    }
  }
  g_mocktail_host_sync_ready.store(true, std::memory_order_release);
}

bool mocktail_host_sync_tables_ready() {
  if (g_mocktail_host_sync_ready.load(std::memory_order_acquire)) {
    return true;
  }
  const int once_result = ::pthread_once(&g_mocktail_host_sync_once,
                                         mocktail_initialize_host_sync_tables);
  return once_result == 0 &&
         g_mocktail_host_sync_ready.load(std::memory_order_acquire) &&
         g_mocktail_host_sync_init_result.load(std::memory_order_acquire) == 0;
}

size_t mocktail_host_sync_bucket(uintptr_t guest_address) {
  uintptr_t key = guest_address >> 3;
  key ^= key >> 17;
  key *= static_cast<uintptr_t>(0xed5ad4bbU);
  key ^= key >> 11;
  return static_cast<size_t>(key) & (kHostSyncBucketCount - 1);
}

int mocktail_bionic_mutex_type_from_storage(
    const pthread_mutex_t* guest_mutex) {
  uint16_t guest_state = 0;
  std::memcpy(&guest_state, guest_mutex, sizeof(guest_state));
  if (guest_state == kBionicMutexDestroyedState) {
    return -1;
  }
  const int type =
      (guest_state >> kBionicMutexTypeShift) & kBionicMutexTypeMask;
  return type <= kBionicMutexErrorCheck ? type : -1;
}

int mocktail_bionic_mutex_type_from_attributes(
    const MocktailBionicMutexAttr* guest_attributes) {
  if (guest_attributes == nullptr) {
    return kBionicMutexNormal;
  }
  int64_t attributes = 0;
  std::memcpy(&attributes, guest_attributes, sizeof(attributes));
  const int type = static_cast<int>(attributes & kBionicMutexAttrTypeMask);
  return type <= kBionicMutexErrorCheck ? type : -1;
}

void mocktail_store_bionic_mutex_attributes(MocktailBionicMutexAttr* guest_attr,
                                            int64_t value) {
  std::memcpy(guest_attr, &value, sizeof(value));
}

void mocktail_initialize_bionic_mutex_storage(pthread_mutex_t* guest_mutex,
                                              int type) {
  std::memset(guest_mutex, 0, kBionicMutexSize);
  const uint16_t guest_state = static_cast<uint16_t>(
      (type & kBionicMutexTypeMask) << kBionicMutexTypeShift);
  std::memcpy(guest_mutex, &guest_state, sizeof(guest_state));
}

void mocktail_mark_bionic_mutex_destroyed(pthread_mutex_t* guest_mutex) {
  const uint16_t guest_state = kBionicMutexDestroyedState;
  std::memcpy(guest_mutex, &guest_state, sizeof(guest_state));
}

int mocktail_host_mutex_type(int bionic_type) {
  switch (bionic_type) {
    case kBionicMutexNormal:
      // Bionic NORMAL intentionally performs no owner check. The supported
      // glibc and musl NORMAL implementations preserve that behavior.
      return PTHREAD_MUTEX_NORMAL;
    case kBionicMutexRecursive:
      return PTHREAD_MUTEX_RECURSIVE;
    case kBionicMutexErrorCheck:
      return PTHREAD_MUTEX_ERRORCHECK;
    default:
      return -1;
  }
}

MocktailHostMutexEntry* mocktail_get_host_mutex_entry_slow(
    pthread_mutex_t* guest_mutex, bool create, int requested_bionic_type,
    uintptr_t guest_address, uint64_t cache_epoch,
    MocktailHostMutexCacheEntry* cached) {
  if (!mocktail_host_sync_tables_ready()) {
    return nullptr;
  }

  const size_t bucket_index = mocktail_host_sync_bucket(guest_address);
  pthread_mutex_t* shard =
      &g_mocktail_host_mutex_shards[bucket_index & (kHostSyncShardCount - 1)];
  ::pthread_mutex_lock(shard);
  for (MocktailHostMutexEntry* entry =
           g_mocktail_host_mutex_buckets[bucket_index];
       entry != nullptr; entry = entry->next) {
    if (entry->guest_address == guest_address) {
      ::pthread_mutex_unlock(shard);
      *cached = {guest_address, entry, cache_epoch};
      return entry;
    }
  }

  if (!create) {
    ::pthread_mutex_unlock(shard);
    return nullptr;
  }

  auto* entry = new (std::nothrow) MocktailHostMutexEntry;
  if (entry == nullptr) {
    ::pthread_mutex_unlock(shard);
    return nullptr;
  }

  const int bionic_type = requested_bionic_type >= 0
                              ? requested_bionic_type
                              : mocktail_bionic_mutex_type_from_storage(
                                    guest_mutex);
  const int host_type = mocktail_host_mutex_type(bionic_type);
  pthread_mutexattr_t host_attr;
  int init_result = host_type < 0 ? EINVAL
                                  : ::pthread_mutexattr_init(&host_attr);
  const bool host_attr_initialized = init_result == 0;
  if (init_result == 0) {
    init_result = ::pthread_mutexattr_settype(&host_attr, host_type);
  }
  if (init_result == 0) {
    init_result = ::pthread_mutex_init(&entry->host_mutex, &host_attr);
  }
  if (host_attr_initialized) {
    ::pthread_mutexattr_destroy(&host_attr);
  }
  if (init_result != 0) {
    delete entry;
    ::pthread_mutex_unlock(shard);
    return nullptr;
  }

  entry->guest_address = guest_address;
  entry->bionic_type = bionic_type;
  entry->next = g_mocktail_host_mutex_buckets[bucket_index];
  g_mocktail_host_mutex_buckets[bucket_index] = entry;
  ::pthread_mutex_unlock(shard);
  *cached = {guest_address, entry, cache_epoch};
  return entry;
}

__attribute__((always_inline)) inline MocktailHostMutexEntry*
mocktail_get_host_mutex_entry(pthread_mutex_t* guest_mutex, bool create,
                              int requested_bionic_type = -1) {
  if (guest_mutex == nullptr || mocktail_is_low_bionic_pointer(guest_mutex)) {
    return nullptr;
  }
  const uintptr_t guest_address = reinterpret_cast<uintptr_t>(guest_mutex);
  const uint64_t cache_epoch =
      g_mocktail_host_mutex_cache_epoch.load(std::memory_order_acquire);
  MocktailHostMutexCacheEntry& cached =
      g_mocktail_host_mutex_cache[(guest_address >> 3) &
                                  (kHostMutexThreadCacheCount - 1)];
  if (cached.epoch == cache_epoch &&
      cached.guest_address == guest_address) {
    return cached.host_entry;
  }
  return mocktail_get_host_mutex_entry_slow(
      guest_mutex, create, requested_bionic_type, guest_address, cache_epoch,
      &cached);
}

void mocktail_record_host_mutex_lock(MocktailHostMutexEntry* entry) {
  if (entry->bionic_type == kBionicMutexRecursive) {
    entry->held_depth.fetch_add(1, std::memory_order_relaxed);
  }
}

void mocktail_record_host_mutex_unlock(MocktailHostMutexEntry* entry) {
  if (entry->bionic_type == kBionicMutexRecursive) {
    entry->held_depth.fetch_sub(1, std::memory_order_relaxed);
  }
}

pthread_cond_t* mocktail_get_host_cond(
    pthread_cond_t* guest_cond, bool create,
    const pthread_condattr_t* host_attributes = nullptr) {
  if (guest_cond == nullptr || mocktail_is_low_bionic_pointer(guest_cond)) {
    return nullptr;
  }
  if (!mocktail_host_sync_tables_ready()) {
    return nullptr;
  }

  const uintptr_t guest_address = reinterpret_cast<uintptr_t>(guest_cond);
  const size_t bucket_index = mocktail_host_sync_bucket(guest_address);
  pthread_mutex_t* shard =
      &g_mocktail_host_cond_shards[bucket_index & (kHostSyncShardCount - 1)];
  ::pthread_mutex_lock(shard);
  for (MocktailHostCondEntry* entry =
           g_mocktail_host_cond_buckets[bucket_index];
       entry != nullptr; entry = entry->next) {
    if (entry->guest_address == guest_address) {
      ::pthread_mutex_unlock(shard);
      return &entry->host_cond;
    }
  }

  if (!create) {
    ::pthread_mutex_unlock(shard);
    return nullptr;
  }

  auto* entry = static_cast<MocktailHostCondEntry*>(
      std::calloc(1, sizeof(MocktailHostCondEntry)));
  if (entry == nullptr) {
    ::pthread_mutex_unlock(shard);
    return nullptr;
  }

  int init_result =
      ::pthread_cond_init(&entry->host_cond, host_attributes);
  if (init_result != 0) {
    std::free(entry);
    ::pthread_mutex_unlock(shard);
    return nullptr;
  }

  entry->guest_address = guest_address;
  entry->next = g_mocktail_host_cond_buckets[bucket_index];
  g_mocktail_host_cond_buckets[bucket_index] = entry;
  ::pthread_mutex_unlock(shard);
  return &entry->host_cond;
}

int mocktail_pthread_condattr_init(pthread_condattr_t* attr) {
  if (attr == nullptr || mocktail_is_low_bionic_pointer(attr)) {
    return EINVAL;
  }
  return ::pthread_condattr_init(attr);
}
int mocktail_pthread_condattr_destroy(pthread_condattr_t* attr) {
  if (attr == nullptr || mocktail_is_low_bionic_pointer(attr)) {
    return EINVAL;
  }
  return ::pthread_condattr_destroy(attr);
}
int mocktail_pthread_condattr_setclock(pthread_condattr_t* attr,
                                       clockid_t clock_id) {
  if (attr == nullptr || mocktail_is_low_bionic_pointer(attr)) {
    return EINVAL;
  }
  return ::pthread_condattr_setclock(attr, clock_id);
}
int mocktail_pthread_cond_init(pthread_cond_t* cond,
                               const pthread_condattr_t* attr) {
  if (mocktail_is_low_bionic_pointer(cond)) {
    return 0;
  }
  return mocktail_get_host_cond(cond, true, attr) != nullptr ? 0 : ENOMEM;
}
int mocktail_pthread_cond_destroy(pthread_cond_t* cond) {
  if (mocktail_is_low_bionic_pointer(cond)) {
    return 0;
  }
  if (!mocktail_host_sync_tables_ready()) {
    return ENOMEM;
  }
  const uintptr_t guest_address = reinterpret_cast<uintptr_t>(cond);
  const size_t bucket_index = mocktail_host_sync_bucket(guest_address);
  pthread_mutex_t* shard =
      &g_mocktail_host_cond_shards[bucket_index & (kHostSyncShardCount - 1)];
  ::pthread_mutex_lock(shard);
  MocktailHostCondEntry** link = &g_mocktail_host_cond_buckets[bucket_index];
  while (*link != nullptr) {
    MocktailHostCondEntry* entry = *link;
    if (entry->guest_address == guest_address) {
      int destroy_result = ::pthread_cond_destroy(&entry->host_cond);
      if (destroy_result == EBUSY) {
        ::pthread_mutex_unlock(shard);
        return 0;
      }
      *link = entry->next;
      ::pthread_mutex_unlock(shard);
      std::free(entry);
      return destroy_result;
    }
    link = &entry->next;
  }
  ::pthread_mutex_unlock(shard);
  return 0;
}
int mocktail_pthread_cond_signal(pthread_cond_t* cond) {
  if (mocktail_is_low_bionic_pointer(cond)) {
    return 0;
  }
  pthread_cond_t* host_cond = mocktail_get_host_cond(cond, true);
  return host_cond == nullptr ? EINVAL : ::pthread_cond_signal(host_cond);
}
int mocktail_pthread_cond_broadcast(pthread_cond_t* cond) {
  if (mocktail_is_low_bionic_pointer(cond)) {
    return 0;
  }
  pthread_cond_t* host_cond = mocktail_get_host_cond(cond, true);
  return host_cond == nullptr ? EINVAL : ::pthread_cond_broadcast(host_cond);
}
int mocktail_pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) {
  if (mocktail_is_low_bionic_pointer(cond) ||
      mocktail_is_low_bionic_pointer(mutex)) {
    return 0;
  }
  pthread_cond_t* host_cond = mocktail_get_host_cond(cond, true);
  MocktailHostMutexEntry* host_entry =
      mocktail_get_host_mutex_entry(mutex, true);
  if (host_cond == nullptr || host_entry == nullptr) {
    return EINVAL;
  }
  return ::pthread_cond_wait(host_cond, &host_entry->host_mutex);
}
int mocktail_pthread_cond_timedwait(pthread_cond_t* cond,
                                    pthread_mutex_t* mutex,
                                    const timespec* abstime) {
  if (mocktail_is_low_bionic_pointer(cond) ||
      mocktail_is_low_bionic_pointer(mutex)) {
    return ETIMEDOUT;
  }
  pthread_cond_t* host_cond = mocktail_get_host_cond(cond, true);
  MocktailHostMutexEntry* host_entry =
      mocktail_get_host_mutex_entry(mutex, true);
  if (host_cond == nullptr || host_entry == nullptr) {
    return EINVAL;
  }
  return ::pthread_cond_timedwait(host_cond, &host_entry->host_mutex, abstime);
}

int mocktail_pthread_mutexattr_init(MocktailBionicMutexAttr* attr) {
  if (attr == nullptr || mocktail_is_low_bionic_pointer(attr)) {
    return EINVAL;
  }
  mocktail_store_bionic_mutex_attributes(attr, kBionicMutexNormal);
  return 0;
}
int mocktail_pthread_mutexattr_destroy(MocktailBionicMutexAttr* attr) {
  if (attr == nullptr || mocktail_is_low_bionic_pointer(attr)) {
    return EINVAL;
  }
  mocktail_store_bionic_mutex_attributes(attr, -1);
  return 0;
}
int mocktail_pthread_mutexattr_settype(MocktailBionicMutexAttr* attr, int type) {
  if (attr == nullptr || mocktail_is_low_bionic_pointer(attr) ||
      type < kBionicMutexNormal || type > kBionicMutexErrorCheck) {
    return EINVAL;
  }
  int64_t attributes = 0;
  std::memcpy(&attributes, attr, sizeof(attributes));
  attributes = (attributes & ~kBionicMutexAttrTypeMask) | type;
  mocktail_store_bionic_mutex_attributes(attr, attributes);
  return 0;
}

int mocktail_pthread_mutex_init(pthread_mutex_t* mutex,
                                const MocktailBionicMutexAttr* attr) {
  if (mocktail_is_low_bionic_pointer(mutex)) {
    return 0;
  }
  if (attr != nullptr && mocktail_is_low_bionic_pointer(attr)) {
    return EINVAL;
  }
  const int bionic_type =
      mocktail_bionic_mutex_type_from_attributes(attr);
  if (bionic_type < 0) {
    return EINVAL;
  }
  mocktail_initialize_bionic_mutex_storage(mutex, bionic_type);
  return mocktail_get_host_mutex_entry(mutex, true, bionic_type) != nullptr
             ? 0
             : ENOMEM;
}
int mocktail_pthread_mutex_destroy(pthread_mutex_t* mutex) {
  if (mocktail_is_low_bionic_pointer(mutex)) {
    return 0;
  }
  if (!mocktail_host_sync_tables_ready()) {
    return ENOMEM;
  }
  const uintptr_t guest_address = reinterpret_cast<uintptr_t>(mutex);
  const size_t bucket_index = mocktail_host_sync_bucket(guest_address);
  pthread_mutex_t* shard =
      &g_mocktail_host_mutex_shards[bucket_index & (kHostSyncShardCount - 1)];
  ::pthread_mutex_lock(shard);
  MocktailHostMutexEntry** link = &g_mocktail_host_mutex_buckets[bucket_index];
  while (*link != nullptr) {
    MocktailHostMutexEntry* entry = *link;
    if (entry->guest_address == guest_address) {
      // musl accepts destroying a locked normal mutex, while Bionic returns
      // EBUSY and leaves it usable. Probe normal/error-check mutexes instead
      // of paying atomic ownership accounting on every lock and unlock.
      // Recursive mutexes need explicit depth because their owner can acquire
      // them again through trylock. Concurrent destroy/use is undefined by
      // POSIX and is intentionally outside this adapter's lifetime contract.
      if (entry->bionic_type == kBionicMutexRecursive &&
          entry->held_depth.load(std::memory_order_relaxed) != 0) {
        ::pthread_mutex_unlock(shard);
        return EBUSY;
      }
      if (entry->bionic_type != kBionicMutexRecursive) {
        const int probe_result =
            ::pthread_mutex_trylock(&entry->host_mutex);
        if (probe_result == 0) {
          ::pthread_mutex_unlock(&entry->host_mutex);
        } else if (probe_result == EBUSY || probe_result == EDEADLK) {
          ::pthread_mutex_unlock(shard);
          return EBUSY;
        } else {
          ::pthread_mutex_unlock(shard);
          return probe_result;
        }
      }
      // Invalidate every per-thread address cache before destroying storage.
      // POSIX already makes concurrent destroy/use undefined; the epoch also
      // guarantees that later reuse of the same guest address cannot retain a
      // stale host pointer on another thread.
      g_mocktail_host_mutex_cache_epoch.fetch_add(
          1, std::memory_order_acq_rel);
      const int destroy_result = ::pthread_mutex_destroy(&entry->host_mutex);
      if (destroy_result != 0) {
        ::pthread_mutex_unlock(shard);
        return destroy_result;
      }
      *link = entry->next;
      mocktail_mark_bionic_mutex_destroyed(mutex);
      ::pthread_mutex_unlock(shard);
      delete entry;
      return 0;
    }
    link = &entry->next;
  }
  ::pthread_mutex_unlock(shard);
  mocktail_mark_bionic_mutex_destroyed(mutex);
  return 0;
}
int mocktail_pthread_mutex_lock(pthread_mutex_t* mutex) {
  if (mocktail_is_low_bionic_pointer(mutex)) {
    return 0;
  }
  MocktailHostMutexEntry* host_entry =
      mocktail_get_host_mutex_entry(mutex, true);
  if (host_entry == nullptr) {
    return EINVAL;
  }
  const int lock_result = ::pthread_mutex_lock(&host_entry->host_mutex);
  if (lock_result == 0) {
    mocktail_record_host_mutex_lock(host_entry);
  }
  return lock_result;
}
int mocktail_pthread_mutex_trylock(pthread_mutex_t* mutex) {
  if (mocktail_is_low_bionic_pointer(mutex)) {
    return 0;
  }
  MocktailHostMutexEntry* host_entry =
      mocktail_get_host_mutex_entry(mutex, true);
  if (host_entry == nullptr) {
    return EINVAL;
  }
  const int lock_result = ::pthread_mutex_trylock(&host_entry->host_mutex);
  if (lock_result == 0) {
    mocktail_record_host_mutex_lock(host_entry);
  }
  return lock_result;
}
int mocktail_pthread_mutex_unlock(pthread_mutex_t* mutex) {
  if (mocktail_is_low_bionic_pointer(mutex)) {
    return 0;
  }
  // Create on unlock if missing: guest code may touch zero-init Bionic mutex
  // storage without pthread_mutex_init when constructors were skipped.
  MocktailHostMutexEntry* host_entry =
      mocktail_get_host_mutex_entry(mutex, true);
  if (host_entry == nullptr) {
    return 0;
  }
  const int unlock_result = ::pthread_mutex_unlock(&host_entry->host_mutex);
  if (unlock_result == 0) {
    mocktail_record_host_mutex_unlock(host_entry);
    mocktail::compat::ApplyHttpClientSpinGuard();
  }
  return unlock_result;
}
int __poll_chk(struct pollfd* fds, nfds_t nfds, int timeout, size_t fds_len) {
  if (nfds > fds_len / sizeof(struct pollfd)) {
    errno = ERANGE;
    nfds = static_cast<nfds_t>(fds_len / sizeof(struct pollfd));
  }
  return CallHostPoll(fds, nfds, timeout);
}
ssize_t __sendto_chk(int socket_fd, const void* buf, size_t len, size_t buf_len,
                     int flags, const struct sockaddr* dest,
                     socklen_t dest_len) {
  if (len > buf_len) {
    errno = ERANGE;
    len = buf_len;
  }
  return CallHostSendto(socket_fd, buf, len, flags, dest, dest_len);
}
void __FD_SET_chk(int fd, fd_set* set, size_t set_len) {
  if (set_len < sizeof(fd_set)) {
    errno = ERANGE;
    return;
  }
  FD_SET(fd, set);
}
void __FD_CLR_chk(int fd, fd_set* set, size_t set_len) {
  if (set_len < sizeof(fd_set)) {
    errno = ERANGE;
    return;
  }
  FD_CLR(fd, set);
}
int __FD_ISSET_chk(int fd, fd_set* set, size_t set_len) {
  if (set_len < sizeof(fd_set)) {
    errno = ERANGE;
    return 0;
  }
  return FD_ISSET(fd, set);
}
extern void* mocktail_gameactivity_on_start_native;
extern void* mocktail_gameactivity_on_resume_native;
extern void* mocktail_gameactivity_on_surface_created_native;
extern void* mocktail_gameactivity_on_surface_changed_native;
extern void* mocktail_gameactivity_on_surface_redraw_needed_native;
}  // extern "C"
