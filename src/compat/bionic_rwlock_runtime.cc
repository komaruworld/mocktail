#include "compat/bionic_rwlock_runtime.h"

#include <errno.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <climits>
#include <cstdint>
#include <cstring>

namespace {

// Bionic rwlock state word:
//   bit 31     writer owns
//   bits 30-2  reader count
//   bit 1      pending writers
//   bit 0      pending readers
constexpr int kRwlockWriterFlag = 1 << 31;
constexpr int kRwlockPendingWritersFlag = 1 << 1;
constexpr int kRwlockPendingReadersFlag = 1 << 0;
constexpr int kRwlockReaderStep = 1 << 2;
constexpr int kRwlockPendingMask =
    kRwlockPendingWritersFlag | kRwlockPendingReadersFlag;

int* RwlockState(pthread_rwlock_t* rwlock) {
  return reinterpret_cast<int*>(rwlock);
}

int32_t* RwlockWriterTid(pthread_rwlock_t* rwlock) {
  return reinterpret_cast<int32_t*>(reinterpret_cast<char*>(rwlock) + 4);
}

pid_t RwlockTid() {
  thread_local pid_t tid = 0;
  if (__builtin_expect(tid == 0, 0)) {
    tid = ::gettid();
  }
  return tid;
}

int RwlockFutex(int* state, int op, int val) {
  return static_cast<int>(::syscall(SYS_futex, state, op | FUTEX_PRIVATE_FLAG,
                                    val, nullptr, nullptr, 0));
}

bool OwnedByWriter(int state) {
  return state < 0;
}

bool OwnedByReaders(int state) {
  return state >= kRwlockReaderStep;
}

int TryRdlock(int* state) {
  int old_state = __atomic_load_n(state, __ATOMIC_RELAXED);
  while (!OwnedByWriter(old_state)) {
    const int new_state = old_state + kRwlockReaderStep;
    if (!OwnedByReaders(new_state)) {
      return EAGAIN;
    }
    if (__atomic_compare_exchange_n(state, &old_state, new_state, true,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
      return 0;
    }
  }
  return EBUSY;
}

int TryWrlock(pthread_rwlock_t* rwlock) {
  int* state = RwlockState(rwlock);
  int old_state = __atomic_load_n(state, __ATOMIC_RELAXED);
  while (!OwnedByWriter(old_state) && !OwnedByReaders(old_state)) {
    const int new_state = old_state | kRwlockWriterFlag;
    if (__atomic_compare_exchange_n(state, &old_state, new_state, true,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
      __atomic_store_n(RwlockWriterTid(rwlock), static_cast<int32_t>(RwlockTid()),
                       __ATOMIC_RELAXED);
      return 0;
    }
  }
  return EBUSY;
}

}  // namespace

extern "C" {

int mocktail_bionic_pthread_rwlock_init(
    pthread_rwlock_t* rwlock, const pthread_rwlockattr_t* /*attr*/) {
  if (rwlock == nullptr ||
      reinterpret_cast<uintptr_t>(rwlock) < 4096) {
    return EINVAL;
  }
  std::memset(rwlock, 0, sizeof(*rwlock));
  return 0;
}

int mocktail_bionic_pthread_rwlock_destroy(pthread_rwlock_t* rwlock) {
  if (rwlock == nullptr) {
    return EINVAL;
  }
  const int state = __atomic_load_n(RwlockState(rwlock), __ATOMIC_RELAXED);
  if ((state & ~kRwlockPendingMask) != 0) {
    return EBUSY;
  }
  return 0;
}

int mocktail_bionic_pthread_rwlock_rdlock(pthread_rwlock_t* rwlock) {
  if (rwlock == nullptr ||
      reinterpret_cast<uintptr_t>(rwlock) < 4096) {
    return EINVAL;
  }
  int* state = RwlockState(rwlock);
  if (TryRdlock(state) == 0) {
    return 0;
  }
  for (;;) {
    const int result = TryRdlock(state);
    if (result == 0 || result == EAGAIN) {
      return result;
    }
    int old_state = __atomic_load_n(state, __ATOMIC_RELAXED);
    if (!OwnedByWriter(old_state)) {
      continue;
    }
    const int pending = old_state | kRwlockPendingReadersFlag;
    if (!__atomic_compare_exchange_n(state, &old_state, pending, true,
                                     __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
      continue;
    }
    (void)RwlockFutex(state, FUTEX_WAIT, pending);
  }
}

int mocktail_bionic_pthread_rwlock_wrlock(pthread_rwlock_t* rwlock) {
  if (rwlock == nullptr ||
      reinterpret_cast<uintptr_t>(rwlock) < 4096) {
    return EINVAL;
  }
  if (__atomic_load_n(RwlockWriterTid(rwlock), __ATOMIC_RELAXED) ==
      static_cast<int32_t>(RwlockTid())) {
    return EDEADLK;
  }
  if (TryWrlock(rwlock) == 0) {
    return 0;
  }
  int* state = RwlockState(rwlock);
  for (;;) {
    if (TryWrlock(rwlock) == 0) {
      return 0;
    }
    int old_state = __atomic_load_n(state, __ATOMIC_RELAXED);
    if (!OwnedByWriter(old_state) && !OwnedByReaders(old_state)) {
      continue;
    }
    const int pending = old_state | kRwlockPendingWritersFlag;
    if (!__atomic_compare_exchange_n(state, &old_state, pending, true,
                                     __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
      continue;
    }
    (void)RwlockFutex(state, FUTEX_WAIT, pending);
  }
}

int mocktail_bionic_pthread_rwlock_unlock(pthread_rwlock_t* rwlock) {
  if (rwlock == nullptr ||
      reinterpret_cast<uintptr_t>(rwlock) < 4096) {
    return EINVAL;
  }
  int* state = RwlockState(rwlock);
  int old_state = __atomic_load_n(state, __ATOMIC_RELAXED);
  if (OwnedByWriter(old_state)) {
    if (__atomic_load_n(RwlockWriterTid(rwlock), __ATOMIC_RELAXED) !=
        static_cast<int32_t>(RwlockTid())) {
      return EPERM;
    }
    __atomic_store_n(RwlockWriterTid(rwlock), 0, __ATOMIC_RELAXED);
    old_state = __atomic_fetch_and(state, ~kRwlockWriterFlag, __ATOMIC_RELEASE);
  } else if (OwnedByReaders(old_state)) {
    old_state =
        __atomic_fetch_sub(state, kRwlockReaderStep, __ATOMIC_RELEASE);
    if ((old_state >> 2) != 1) {
      return 0;
    }
  } else {
    return EPERM;
  }
  (void)RwlockFutex(state, FUTEX_WAKE, INT_MAX);
  return 0;
}

}  // extern "C"
