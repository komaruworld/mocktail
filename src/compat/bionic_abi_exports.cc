#include "compat/bionic_abi_exports.h"

#include <linux/futex.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

// Android x86-64 pthread_mutex_t is a 40-byte Bionic object: uint16_t state
// at offset 0 (type in bits 15:14, shared in 13, recursive counter in 12:2,
// lock state in 1:0), owner tid at offset 4. libroblox inlines that futex
// path, so lock/unlock must use the guest word — not a host pthread_mutex_t.
constexpr size_t kBionicMutexSize = 40;
constexpr uint16_t kBionicMutexDestroyedState = 0xffff;
constexpr uint16_t kBionicMutexTypeShift = 14;
constexpr uint16_t kBionicMutexTypeMask = 0x3;
constexpr uint16_t kBionicMutexSharedMask = 1u << 13;
constexpr uint16_t kBionicMutexStateMask = 0x3;
constexpr uint16_t kBionicMutexUnlocked = 0;
constexpr uint16_t kBionicMutexLockedUncontended = 1;
constexpr uint16_t kBionicMutexLockedContended = 2;
constexpr uint16_t kBionicMutexCounterMask = 0x1ffcu;
constexpr uint16_t kBionicMutexCounterOne = 1u << 2;
constexpr uint16_t kBionicMutexTypeFieldMask =
    kBionicMutexTypeMask << kBionicMutexTypeShift;
constexpr uint16_t kBionicMutexTypeBitsNormal = 0;
constexpr uint16_t kBionicMutexTypeBitsRecursive =
    1u << kBionicMutexTypeShift;
constexpr uint16_t kBionicMutexTypeBitsErrorCheck =
    2u << kBionicMutexTypeShift;
constexpr uint16_t kBionicMutexTypeBitsPi = kBionicMutexTypeFieldMask;
constexpr int64_t kBionicMutexAttrTypeMask = 0x0f;
constexpr int kBionicMutexNormal = 0;
constexpr int kBionicMutexRecursive = 1;
constexpr int kBionicMutexErrorCheck = 2;
static_assert(sizeof(pthread_mutex_t) >= kBionicMutexSize);

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

uint16_t* mocktail_bionic_mutex_state(pthread_mutex_t* mutex) {
  return reinterpret_cast<uint16_t*>(mutex);
}

int32_t* mocktail_bionic_mutex_owner(pthread_mutex_t* mutex) {
  return reinterpret_cast<int32_t*>(reinterpret_cast<char*>(mutex) + 4);
}

pid_t mocktail_bionic_tid() {
  thread_local pid_t tid = 0;
  if (__builtin_expect(tid == 0, 0)) {
    tid = ::gettid();
  }
  return tid;
}

void mocktail_initialize_bionic_mutex_storage(pthread_mutex_t* guest_mutex,
                                              int type) {
  std::memset(guest_mutex, 0, kBionicMutexSize);
  const uint16_t guest_state = static_cast<uint16_t>(
      (type & kBionicMutexTypeMask) << kBionicMutexTypeShift);
  __atomic_store_n(mocktail_bionic_mutex_state(guest_mutex), guest_state,
                   __ATOMIC_RELAXED);
}

int mocktail_bionic_futex(uint32_t* addr, int op, uint32_t val,
                          const timespec* timeout, uint32_t bitset);

int mocktail_bionic_mutex_futex(pthread_mutex_t* mutex, int wait_op,
                                uint16_t expected, bool shared) {
  uint32_t* word = reinterpret_cast<uint32_t*>(mutex);
  const int op = shared ? wait_op : (wait_op | FUTEX_PRIVATE_FLAG);
  return mocktail_bionic_futex(word, op, expected, nullptr, 0);
}

int mocktail_bionic_normal_mutex_trylock(uint16_t* state, uint16_t shared) {
  const uint16_t unlocked = static_cast<uint16_t>(shared | kBionicMutexUnlocked);
  const uint16_t locked =
      static_cast<uint16_t>(shared | kBionicMutexLockedUncontended);
  uint16_t old_state = unlocked;
  if (__atomic_compare_exchange_n(state, &old_state, locked, false,
                                  __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
    return 0;
  }
  return EBUSY;
}

int mocktail_bionic_normal_mutex_lock(pthread_mutex_t* mutex, uint16_t shared) {
  uint16_t* state = mocktail_bionic_mutex_state(mutex);
  if (mocktail_bionic_normal_mutex_trylock(state, shared) == 0) {
    return 0;
  }
  // Park on the guest word. Do not CAS-spin: this host has four threads and
  // a spinning locker steals a worker/render core.
  const uint16_t unlocked = static_cast<uint16_t>(shared | kBionicMutexUnlocked);
  const uint16_t contended =
      static_cast<uint16_t>(shared | kBionicMutexLockedContended);
  for (;;) {
    const uint16_t old_state =
        __atomic_exchange_n(state, contended, __ATOMIC_ACQUIRE);
    if (old_state == unlocked) {
      return 0;
    }
    (void)mocktail_bionic_mutex_futex(mutex, FUTEX_WAIT, contended,
                                      shared != 0);
  }
}

void mocktail_bionic_normal_mutex_unlock(pthread_mutex_t* mutex,
                                         uint16_t shared) {
  uint16_t* state = mocktail_bionic_mutex_state(mutex);
  const uint16_t unlocked = static_cast<uint16_t>(shared | kBionicMutexUnlocked);
  const uint16_t contended =
      static_cast<uint16_t>(shared | kBionicMutexLockedContended);
  const uint16_t old_state =
      __atomic_exchange_n(state, unlocked, __ATOMIC_RELEASE);
  if (old_state == contended) {
    (void)mocktail_bionic_mutex_futex(mutex, FUTEX_WAKE, 1, shared != 0);
  }
}

int mocktail_bionic_recursive_increment(uint16_t* state, uint16_t old_state) {
  if ((old_state & kBionicMutexCounterMask) == kBionicMutexCounterMask) {
    return EAGAIN;
  }
  __atomic_fetch_add(state, kBionicMutexCounterOne, __ATOMIC_RELAXED);
  return 0;
}

int mocktail_bionic_owned_mutex_lock(pthread_mutex_t* mutex, uint16_t old_state) {
  uint16_t* state = mocktail_bionic_mutex_state(mutex);
  int32_t* owner = mocktail_bionic_mutex_owner(mutex);
  const uint16_t mtype =
      static_cast<uint16_t>(old_state & kBionicMutexTypeFieldMask);
  const uint16_t shared =
      static_cast<uint16_t>(old_state & kBionicMutexSharedMask);
  const pid_t tid = mocktail_bionic_tid();
  if (tid == __atomic_load_n(owner, __ATOMIC_RELAXED)) {
    if (mtype == kBionicMutexTypeBitsErrorCheck) {
      return EDEADLK;
    }
    return mocktail_bionic_recursive_increment(state, old_state);
  }

  const uint16_t unlocked =
      static_cast<uint16_t>(mtype | shared | kBionicMutexUnlocked);
  const uint16_t locked_uncontended =
      static_cast<uint16_t>(mtype | shared | kBionicMutexLockedUncontended);
  const uint16_t locked_contended =
      static_cast<uint16_t>(mtype | shared | kBionicMutexLockedContended);

  if (old_state == unlocked) {
    if (__atomic_compare_exchange_n(state, &old_state, locked_uncontended,
                                    false, __ATOMIC_ACQUIRE,
                                    __ATOMIC_RELAXED)) {
      __atomic_store_n(owner, tid, __ATOMIC_RELAXED);
      return 0;
    }
  }

  for (;;) {
    if (old_state == unlocked) {
      if (__atomic_compare_exchange_n(state, &old_state, locked_contended, true,
                                      __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        __atomic_store_n(owner, tid, __ATOMIC_RELAXED);
        return 0;
      }
      continue;
    }
    if ((old_state & kBionicMutexStateMask) == kBionicMutexLockedUncontended) {
      const uint16_t contended =
          static_cast<uint16_t>(old_state ^ (kBionicMutexLockedContended ^
                                             kBionicMutexLockedUncontended));
      if (!__atomic_compare_exchange_n(state, &old_state, contended, true,
                                       __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
        continue;
      }
      old_state = contended;
    }
    (void)mocktail_bionic_mutex_futex(mutex, FUTEX_WAIT, old_state,
                                      shared != 0);
    old_state = __atomic_load_n(state, __ATOMIC_RELAXED);
  }
}

int mocktail_bionic_owned_mutex_trylock(pthread_mutex_t* mutex,
                                        uint16_t old_state) {
  uint16_t* state = mocktail_bionic_mutex_state(mutex);
  int32_t* owner = mocktail_bionic_mutex_owner(mutex);
  const uint16_t mtype =
      static_cast<uint16_t>(old_state & kBionicMutexTypeFieldMask);
  const pid_t tid = mocktail_bionic_tid();
  if (tid == __atomic_load_n(owner, __ATOMIC_RELAXED)) {
    if (mtype == kBionicMutexTypeBitsErrorCheck) {
      return EBUSY;
    }
    return mocktail_bionic_recursive_increment(state, old_state);
  }
  const uint16_t shared =
      static_cast<uint16_t>(old_state & kBionicMutexSharedMask);
  const uint16_t unlocked =
      static_cast<uint16_t>(mtype | shared | kBionicMutexUnlocked);
  const uint16_t locked =
      static_cast<uint16_t>(mtype | shared | kBionicMutexLockedUncontended);
  old_state = unlocked;
  if (__atomic_compare_exchange_n(state, &old_state, locked, false,
                                  __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
    __atomic_store_n(owner, tid, __ATOMIC_RELAXED);
    return 0;
  }
  return EBUSY;
}

int mocktail_bionic_owned_mutex_unlock(pthread_mutex_t* mutex,
                                       uint16_t old_state) {
  uint16_t* state = mocktail_bionic_mutex_state(mutex);
  int32_t* owner = mocktail_bionic_mutex_owner(mutex);
  const uint16_t mtype =
      static_cast<uint16_t>(old_state & kBionicMutexTypeFieldMask);
  const uint16_t shared =
      static_cast<uint16_t>(old_state & kBionicMutexSharedMask);
  if (mocktail_bionic_tid() != __atomic_load_n(owner, __ATOMIC_RELAXED)) {
    return EPERM;
  }
  if ((old_state & kBionicMutexCounterMask) != 0) {
    __atomic_fetch_sub(state, kBionicMutexCounterOne, __ATOMIC_RELAXED);
    return 0;
  }
  __atomic_store_n(owner, 0, __ATOMIC_RELAXED);
  const uint16_t unlocked =
      static_cast<uint16_t>(mtype | shared | kBionicMutexUnlocked);
  old_state = __atomic_exchange_n(state, unlocked, __ATOMIC_RELEASE);
  if ((old_state & kBionicMutexStateMask) == kBionicMutexLockedContended) {
    (void)mocktail_bionic_mutex_futex(mutex, FUTEX_WAKE, 1, shared != 0);
  }
  return 0;
}

// Bionic condvars are a generation counter + futex on the guest object.
// libroblox inlines that path (syscall(SYS_futex) on the cond word) while
// also importing pthread_cond_*. A separate host pthread_cond_t cannot
// wake the inlined waiters, so wait/signal must use the guest word.
constexpr uint32_t kBionicCondSharedMask = 0x1;
constexpr uint32_t kBionicCondClockMask = 0x2;
constexpr uint32_t kBionicCondCounterStep = 0x4;
constexpr uint32_t kBionicCondFlagsMask =
    kBionicCondSharedMask | kBionicCondClockMask;
constexpr uint32_t kBionicCondDestroyedState = 0xdeadc04d;
constexpr uint32_t kBionicCondAttrDestroyedState = 0xdeada11d;

uint32_t* mocktail_bionic_cond_state(pthread_cond_t* cond) {
  return reinterpret_cast<uint32_t*>(cond);
}

void mocktail_store_bionic_condattr_flags(pthread_condattr_t* attr,
                                          uint32_t flags) {
  std::memset(attr, 0, sizeof(*attr));
  std::memcpy(attr, &flags, sizeof(flags));
}

uint32_t mocktail_load_bionic_condattr_flags(const pthread_condattr_t* attr) {
  uint32_t flags = 0;
  std::memcpy(&flags, attr, sizeof(flags));
  return flags;
}

int mocktail_bionic_futex(uint32_t* addr, int op, uint32_t val,
                          const timespec* timeout, uint32_t bitset) {
  return static_cast<int>(
      ::syscall(SYS_futex, addr, op, val, timeout, nullptr, bitset));
}

int mocktail_bionic_cond_pulse(pthread_cond_t* cond, int waiters) {
  uint32_t* state = mocktail_bionic_cond_state(cond);
  const uint32_t previous =
      __atomic_fetch_add(state, kBionicCondCounterStep, __ATOMIC_RELAXED);
  const int op = (previous & kBionicCondSharedMask) != 0 ? FUTEX_WAKE
                                                         : FUTEX_WAKE_PRIVATE;
  (void)mocktail_bionic_futex(state, op, static_cast<uint32_t>(waiters),
                              nullptr, 0);
  return 0;
}

int mocktail_bionic_cond_timedwait(pthread_cond_t* cond, pthread_mutex_t* mutex,
                                   bool use_realtime_clock,
                                   const timespec* abstime) {
  uint32_t* state = mocktail_bionic_cond_state(cond);
  const uint32_t old_state = __atomic_load_n(state, __ATOMIC_RELAXED);
  const int unlock_result = mocktail_pthread_mutex_unlock(mutex);
  if (unlock_result != 0) {
    return unlock_result;
  }

  const bool shared = (old_state & kBionicCondSharedMask) != 0;
  int wait_result = 0;
  for (;;) {
    int op = 0;
    uint32_t bitset = 0;
    if (abstime == nullptr) {
      op = shared ? FUTEX_WAIT : FUTEX_WAIT_PRIVATE;
    } else {
      op = shared ? FUTEX_WAIT_BITSET
                  : (FUTEX_WAIT_BITSET | FUTEX_PRIVATE_FLAG);
      if (use_realtime_clock) {
        op |= FUTEX_CLOCK_REALTIME;
      }
      bitset = FUTEX_BITSET_MATCH_ANY;
    }
    const int rc =
        mocktail_bionic_futex(state, op, old_state, abstime, bitset);
    if (rc == 0 || errno == EAGAIN) {
      wait_result = 0;
      break;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno == ETIMEDOUT) {
      wait_result = ETIMEDOUT;
      break;
    }
    wait_result = errno != 0 ? errno : EINVAL;
    break;
  }

  const int lock_result = mocktail_pthread_mutex_lock(mutex);
  if (lock_result != 0) {
    return lock_result;
  }
  return wait_result;
}

int mocktail_pthread_condattr_init(pthread_condattr_t* attr) {
  if (attr == nullptr || mocktail_is_low_bionic_pointer(attr)) {
    return EINVAL;
  }
  mocktail_store_bionic_condattr_flags(attr, 0);
  return 0;
}
int mocktail_pthread_condattr_destroy(pthread_condattr_t* attr) {
  if (attr == nullptr || mocktail_is_low_bionic_pointer(attr)) {
    return EINVAL;
  }
  mocktail_store_bionic_condattr_flags(attr, kBionicCondAttrDestroyedState);
  return 0;
}
int mocktail_pthread_condattr_setclock(pthread_condattr_t* attr,
                                       clockid_t clock_id) {
  if (attr == nullptr || mocktail_is_low_bionic_pointer(attr)) {
    return EINVAL;
  }
  if (clock_id != CLOCK_MONOTONIC && clock_id != CLOCK_REALTIME) {
    return EINVAL;
  }
  uint32_t flags = mocktail_load_bionic_condattr_flags(attr);
  flags = (flags & ~kBionicCondClockMask) |
          (static_cast<uint32_t>(clock_id) << 1);
  mocktail_store_bionic_condattr_flags(attr, flags);
  return 0;
}
int mocktail_pthread_cond_init(pthread_cond_t* cond,
                               const pthread_condattr_t* attr) {
  if (mocktail_is_low_bionic_pointer(cond)) {
    return 0;
  }
  if (cond == nullptr) {
    return EINVAL;
  }
  if (attr != nullptr && mocktail_is_low_bionic_pointer(attr)) {
    return EINVAL;
  }
  uint32_t flags = 0;
  if (attr != nullptr) {
    flags = mocktail_load_bionic_condattr_flags(attr) & kBionicCondFlagsMask;
  }
  std::memset(cond, 0, sizeof(*cond));
  __atomic_store_n(mocktail_bionic_cond_state(cond), flags, __ATOMIC_RELAXED);
  return 0;
}
int mocktail_pthread_cond_destroy(pthread_cond_t* cond) {
  if (mocktail_is_low_bionic_pointer(cond)) {
    return 0;
  }
  if (cond == nullptr) {
    return EINVAL;
  }
  __atomic_store_n(mocktail_bionic_cond_state(cond), kBionicCondDestroyedState,
                   __ATOMIC_RELAXED);
  return 0;
}
int mocktail_pthread_cond_signal(pthread_cond_t* cond) {
  if (mocktail_is_low_bionic_pointer(cond)) {
    return 0;
  }
  if (cond == nullptr) {
    return EINVAL;
  }
  return mocktail_bionic_cond_pulse(cond, 1);
}
int mocktail_pthread_cond_broadcast(pthread_cond_t* cond) {
  if (mocktail_is_low_bionic_pointer(cond)) {
    return 0;
  }
  if (cond == nullptr) {
    return EINVAL;
  }
  return mocktail_bionic_cond_pulse(cond, INT_MAX);
}
int mocktail_pthread_cond_wait(pthread_cond_t* cond, pthread_mutex_t* mutex) {
  if (mocktail_is_low_bionic_pointer(cond) ||
      mocktail_is_low_bionic_pointer(mutex)) {
    return 0;
  }
  if (cond == nullptr || mutex == nullptr) {
    return EINVAL;
  }
  return mocktail_bionic_cond_timedwait(cond, mutex, false, nullptr);
}
int mocktail_pthread_cond_timedwait(pthread_cond_t* cond,
                                    pthread_mutex_t* mutex,
                                    const timespec* abstime) {
  if (mocktail_is_low_bionic_pointer(cond) ||
      mocktail_is_low_bionic_pointer(mutex)) {
    return ETIMEDOUT;
  }
  if (cond == nullptr || mutex == nullptr || abstime == nullptr) {
    return EINVAL;
  }
  const uint32_t flags =
      __atomic_load_n(mocktail_bionic_cond_state(cond), __ATOMIC_RELAXED);
  const bool use_realtime_clock = (flags & kBionicCondClockMask) == 0;
  return mocktail_bionic_cond_timedwait(cond, mutex, use_realtime_clock,
                                        abstime);
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
  if (mutex == nullptr) {
    return EINVAL;
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
  return 0;
}
int mocktail_pthread_mutex_destroy(pthread_mutex_t* mutex) {
  if (mocktail_is_low_bionic_pointer(mutex)) {
    return 0;
  }
  if (mutex == nullptr) {
    return EINVAL;
  }
  uint16_t* state = mocktail_bionic_mutex_state(mutex);
  uint16_t old_state = __atomic_load_n(state, __ATOMIC_RELAXED);
  if (old_state == kBionicMutexDestroyedState) {
    return EBUSY;
  }
  if ((old_state & kBionicMutexStateMask) != kBionicMutexUnlocked) {
    return EBUSY;
  }
  if (__atomic_compare_exchange_n(state, &old_state, kBionicMutexDestroyedState,
                                  false, __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    return 0;
  }
  return EBUSY;
}
int mocktail_pthread_mutex_lock(pthread_mutex_t* mutex) {
  if (mocktail_is_low_bionic_pointer(mutex)) {
    return 0;
  }
  if (mutex == nullptr) {
    return EINVAL;
  }
  uint16_t* state = mocktail_bionic_mutex_state(mutex);
  const uint16_t old_state = __atomic_load_n(state, __ATOMIC_RELAXED);
  if (old_state == kBionicMutexDestroyedState) {
    return EBUSY;
  }
  const uint16_t mtype =
      static_cast<uint16_t>(old_state & kBionicMutexTypeFieldMask);
  if (mtype == kBionicMutexTypeBitsPi) {
    return EINVAL;
  }
  if (mtype == kBionicMutexTypeBitsNormal) {
    return mocktail_bionic_normal_mutex_lock(
        mutex, static_cast<uint16_t>(old_state & kBionicMutexSharedMask));
  }
  return mocktail_bionic_owned_mutex_lock(mutex, old_state);
}
int mocktail_pthread_mutex_trylock(pthread_mutex_t* mutex) {
  if (mocktail_is_low_bionic_pointer(mutex)) {
    return 0;
  }
  if (mutex == nullptr) {
    return EINVAL;
  }
  uint16_t* state = mocktail_bionic_mutex_state(mutex);
  const uint16_t old_state = __atomic_load_n(state, __ATOMIC_RELAXED);
  if (old_state == kBionicMutexDestroyedState) {
    return EBUSY;
  }
  const uint16_t mtype =
      static_cast<uint16_t>(old_state & kBionicMutexTypeFieldMask);
  if (mtype == kBionicMutexTypeBitsPi) {
    return EINVAL;
  }
  if (mtype == kBionicMutexTypeBitsNormal) {
    return mocktail_bionic_normal_mutex_trylock(
        state, static_cast<uint16_t>(old_state & kBionicMutexSharedMask));
  }
  return mocktail_bionic_owned_mutex_trylock(mutex, old_state);
}
int mocktail_pthread_mutex_unlock(pthread_mutex_t* mutex) {
  if (mocktail_is_low_bionic_pointer(mutex)) {
    return 0;
  }
  if (mutex == nullptr) {
    return 0;
  }
  uint16_t* state = mocktail_bionic_mutex_state(mutex);
  const uint16_t old_state = __atomic_load_n(state, __ATOMIC_RELAXED);
  if (old_state == kBionicMutexDestroyedState) {
    return EBUSY;
  }
  const uint16_t mtype =
      static_cast<uint16_t>(old_state & kBionicMutexTypeFieldMask);
  int unlock_result = 0;
  if (mtype == kBionicMutexTypeBitsPi) {
    unlock_result = EINVAL;
  } else if (mtype == kBionicMutexTypeBitsNormal) {
    mocktail_bionic_normal_mutex_unlock(
        mutex, static_cast<uint16_t>(old_state & kBionicMutexSharedMask));
  } else {
    unlock_result = mocktail_bionic_owned_mutex_unlock(mutex, old_state);
  }
  if (unlock_result == 0) {
    mocktail::compat::ApplyHttpClientSpinGuard();
  }
  return unlock_result;
}

constexpr int kOnceNotStarted = 0;
constexpr int kOnceUnderway = 1;
constexpr int kOnceComplete = 2;
constexpr uint32_t kSpinUnlocked = 0;
constexpr uint32_t kSpinLocked = 1;
constexpr uint32_t kSpinContended = 2;
constexpr uint32_t kBarrierWaitState = 0;
constexpr uint32_t kBarrierReleaseState = 1;

int* mocktail_once_word(pthread_once_t* once_control) {
  return reinterpret_cast<int*>(once_control);
}

int* mocktail_spin_word(pthread_spinlock_t* lock) {
  return const_cast<int*>(lock);
}

uint32_t* mocktail_barrier_init_count(pthread_barrier_t* barrier) {
  return reinterpret_cast<uint32_t*>(barrier);
}

uint32_t* mocktail_barrier_state(pthread_barrier_t* barrier) {
  return reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(barrier) + 4);
}

uint32_t* mocktail_barrier_wait_count(pthread_barrier_t* barrier) {
  return reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(barrier) + 8);
}

int mocktail_pthread_once(pthread_once_t* once_control,
                          void (*init_routine)(void)) {
  if (once_control == nullptr || init_routine == nullptr ||
      mocktail_is_low_bionic_pointer(once_control)) {
    return EINVAL;
  }
  int* word = mocktail_once_word(once_control);
  int old_value = __atomic_load_n(word, __ATOMIC_ACQUIRE);
  for (;;) {
    if (old_value == kOnceComplete) {
      return 0;
    }
    if (!__atomic_compare_exchange_n(word, &old_value, kOnceUnderway, true,
                                     __ATOMIC_ACQUIRE, __ATOMIC_ACQUIRE)) {
      continue;
    }
    if (old_value == kOnceNotStarted) {
      init_routine();
      __atomic_store_n(word, kOnceComplete, __ATOMIC_RELEASE);
      (void)mocktail_bionic_futex(reinterpret_cast<uint32_t*>(word),
                                  FUTEX_WAKE_PRIVATE,
                                  static_cast<uint32_t>(INT_MAX), nullptr, 0);
      return 0;
    }
    (void)mocktail_bionic_futex(reinterpret_cast<uint32_t*>(word),
                                FUTEX_WAIT_PRIVATE,
                                static_cast<uint32_t>(old_value), nullptr, 0);
    old_value = __atomic_load_n(word, __ATOMIC_ACQUIRE);
  }
}

int mocktail_pthread_spin_init(pthread_spinlock_t* lock, int pshared) {
  static_cast<void>(pshared);
  if (lock == nullptr ||
      reinterpret_cast<uintptr_t>(lock) < 4096) {
    return EINVAL;
  }
  __atomic_store_n(mocktail_spin_word(lock), static_cast<int>(kSpinUnlocked),
                   __ATOMIC_RELAXED);
  return 0;
}

int mocktail_pthread_spin_destroy(pthread_spinlock_t* lock) {
  if (lock == nullptr || reinterpret_cast<uintptr_t>(lock) < 4096) {
    return EINVAL;
  }
  int* word = mocktail_spin_word(lock);
  int old_value = static_cast<int>(kSpinUnlocked);
  if (__atomic_compare_exchange_n(word, &old_value,
                                  static_cast<int>(kSpinUnlocked), false,
                                  __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
    return 0;
  }
  return EBUSY;
}

int mocktail_pthread_spin_trylock(pthread_spinlock_t* lock) {
  if (lock == nullptr || reinterpret_cast<uintptr_t>(lock) < 4096) {
    return EINVAL;
  }
  int* word = mocktail_spin_word(lock);
  int old_value = static_cast<int>(kSpinUnlocked);
  if (__atomic_compare_exchange_n(word, &old_value,
                                  static_cast<int>(kSpinLocked), false,
                                  __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
    return 0;
  }
  return EBUSY;
}

int mocktail_pthread_spin_lock(pthread_spinlock_t* lock) {
  if (lock == nullptr || reinterpret_cast<uintptr_t>(lock) < 4096) {
    return EINVAL;
  }
  if (mocktail_pthread_spin_trylock(lock) == 0) {
    return 0;
  }
  int* word = mocktail_spin_word(lock);
  for (;;) {
    const int old_value = __atomic_exchange_n(
        word, static_cast<int>(kSpinContended), __ATOMIC_ACQUIRE);
    if (old_value == static_cast<int>(kSpinUnlocked)) {
      return 0;
    }
    (void)mocktail_bionic_futex(reinterpret_cast<uint32_t*>(word),
                                FUTEX_WAIT_PRIVATE, kSpinContended, nullptr, 0);
  }
}

int mocktail_pthread_spin_unlock(pthread_spinlock_t* lock) {
  if (lock == nullptr || reinterpret_cast<uintptr_t>(lock) < 4096) {
    return EINVAL;
  }
  int* word = mocktail_spin_word(lock);
  const int old_value = __atomic_exchange_n(
      word, static_cast<int>(kSpinUnlocked), __ATOMIC_RELEASE);
  if (old_value == static_cast<int>(kSpinContended)) {
    (void)mocktail_bionic_futex(reinterpret_cast<uint32_t*>(word),
                                FUTEX_WAKE_PRIVATE, 1, nullptr, 0);
  }
  return 0;
}

int mocktail_pthread_barrier_init(pthread_barrier_t* barrier,
                                  const pthread_barrierattr_t* attr,
                                  unsigned count) {
  static_cast<void>(attr);
  if (barrier == nullptr || mocktail_is_low_bionic_pointer(barrier) ||
      count == 0) {
    return EINVAL;
  }
  std::memset(barrier, 0, sizeof(*barrier));
  __atomic_store_n(mocktail_barrier_init_count(barrier), count,
                   __ATOMIC_RELAXED);
  __atomic_store_n(mocktail_barrier_state(barrier), kBarrierWaitState,
                   __ATOMIC_RELAXED);
  __atomic_store_n(mocktail_barrier_wait_count(barrier), 0u, __ATOMIC_RELAXED);
  return 0;
}

int mocktail_pthread_barrier_destroy(pthread_barrier_t* barrier) {
  if (barrier == nullptr || mocktail_is_low_bionic_pointer(barrier)) {
    return EINVAL;
  }
  if (__atomic_load_n(mocktail_barrier_init_count(barrier),
                      __ATOMIC_RELAXED) == 0) {
    return EINVAL;
  }
  uint32_t* state = mocktail_barrier_state(barrier);
  while (__atomic_load_n(state, __ATOMIC_ACQUIRE) == kBarrierReleaseState) {
    (void)mocktail_bionic_futex(state, FUTEX_WAIT_PRIVATE, kBarrierReleaseState,
                                nullptr, 0);
  }
  if (__atomic_load_n(mocktail_barrier_wait_count(barrier),
                      __ATOMIC_RELAXED) != 0) {
    return EBUSY;
  }
  __atomic_store_n(mocktail_barrier_init_count(barrier), 0u, __ATOMIC_RELAXED);
  return 0;
}

int mocktail_pthread_barrier_wait(pthread_barrier_t* barrier) {
  if (barrier == nullptr || mocktail_is_low_bionic_pointer(barrier)) {
    return EINVAL;
  }
  const uint32_t init_count =
      __atomic_load_n(mocktail_barrier_init_count(barrier), __ATOMIC_RELAXED);
  if (init_count == 0) {
    return EINVAL;
  }
  uint32_t* state = mocktail_barrier_state(barrier);
  uint32_t* wait_count = mocktail_barrier_wait_count(barrier);
  while (__atomic_load_n(state, __ATOMIC_ACQUIRE) == kBarrierReleaseState) {
    (void)mocktail_bionic_futex(state, FUTEX_WAIT_PRIVATE, kBarrierReleaseState,
                                nullptr, 0);
  }
  uint32_t prev = __atomic_load_n(wait_count, __ATOMIC_RELAXED);
  for (;;) {
    if (prev >= init_count) {
      return EINVAL;
    }
    if (__atomic_compare_exchange_n(wait_count, &prev, prev + 1u, true,
                                    __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
      break;
    }
  }
  int result = 0;
  if (prev + 1u == init_count) {
    result = PTHREAD_BARRIER_SERIAL_THREAD;
    if (prev != 0) {
      __atomic_store_n(state, kBarrierReleaseState, __ATOMIC_RELEASE);
      (void)mocktail_bionic_futex(state, FUTEX_WAKE_PRIVATE, prev, nullptr, 0);
    }
  } else {
    while (__atomic_load_n(state, __ATOMIC_ACQUIRE) == kBarrierWaitState) {
      (void)mocktail_bionic_futex(state, FUTEX_WAIT_PRIVATE, kBarrierWaitState,
                                  nullptr, 0);
    }
  }
  if (__atomic_fetch_sub(wait_count, 1u, __ATOMIC_RELEASE) == 1u) {
    __atomic_store_n(state, kBarrierWaitState, __ATOMIC_RELEASE);
    (void)mocktail_bionic_futex(state, FUTEX_WAKE_PRIVATE, init_count, nullptr,
                                0);
  }
  return result;
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
extern void* mocktail_gameactivity_on_trim_memory_native;
}  // extern "C"
