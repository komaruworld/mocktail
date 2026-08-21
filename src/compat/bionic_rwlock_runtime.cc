#include "compat/bionic_rwlock_runtime.h"

#include <errno.h>

#include <cstdint>
#include <cstdlib>

namespace {

struct HostRwlockEntry {
  uintptr_t guest_address;
  pthread_rwlock_t host_rwlock;
  HostRwlockEntry* next;
};

pthread_mutex_t g_table_mutex = PTHREAD_MUTEX_INITIALIZER;
HostRwlockEntry* g_table = nullptr;

pthread_rwlock_t* FindOrCreateHostRwlock(pthread_rwlock_t* guest_rwlock,
                                         bool create) noexcept {
  if (guest_rwlock == nullptr ||
      reinterpret_cast<uintptr_t>(guest_rwlock) < 4096) {
    return nullptr;
  }

  const uintptr_t guest_address = reinterpret_cast<uintptr_t>(guest_rwlock);
  ::pthread_mutex_lock(&g_table_mutex);
  for (HostRwlockEntry* entry = g_table; entry != nullptr;
       entry = entry->next) {
    if (entry->guest_address == guest_address) {
      ::pthread_mutex_unlock(&g_table_mutex);
      return &entry->host_rwlock;
    }
  }

  if (!create) {
    ::pthread_mutex_unlock(&g_table_mutex);
    return nullptr;
  }

  auto* entry =
      static_cast<HostRwlockEntry*>(std::calloc(1, sizeof(HostRwlockEntry)));
  if (entry == nullptr) {
    ::pthread_mutex_unlock(&g_table_mutex);
    return nullptr;
  }
  const int init_result = ::pthread_rwlock_init(&entry->host_rwlock, nullptr);
  if (init_result != 0) {
    std::free(entry);
    ::pthread_mutex_unlock(&g_table_mutex);
    return nullptr;
  }

  entry->guest_address = guest_address;
  entry->next = g_table;
  g_table = entry;
  ::pthread_mutex_unlock(&g_table_mutex);
  return &entry->host_rwlock;
}

}  // namespace

extern "C" {

int mocktail_bionic_pthread_rwlock_init(
    pthread_rwlock_t* rwlock, const pthread_rwlockattr_t* /*attr*/) {
  return FindOrCreateHostRwlock(rwlock, true) == nullptr ? ENOMEM : 0;
}

int mocktail_bionic_pthread_rwlock_destroy(pthread_rwlock_t* rwlock) {
  if (rwlock == nullptr) {
    return EINVAL;
  }

  const uintptr_t guest_address = reinterpret_cast<uintptr_t>(rwlock);
  ::pthread_mutex_lock(&g_table_mutex);
  HostRwlockEntry** link = &g_table;
  while (*link != nullptr) {
    HostRwlockEntry* entry = *link;
    if (entry->guest_address == guest_address) {
      *link = entry->next;
      ::pthread_mutex_unlock(&g_table_mutex);
      const int result = ::pthread_rwlock_destroy(&entry->host_rwlock);
      std::free(entry);
      return result;
    }
    link = &entry->next;
  }
  ::pthread_mutex_unlock(&g_table_mutex);
  return 0;
}

int mocktail_bionic_pthread_rwlock_rdlock(pthread_rwlock_t* rwlock) {
  pthread_rwlock_t* host_rwlock = FindOrCreateHostRwlock(rwlock, true);
  return host_rwlock == nullptr ? EINVAL : ::pthread_rwlock_rdlock(host_rwlock);
}

int mocktail_bionic_pthread_rwlock_wrlock(pthread_rwlock_t* rwlock) {
  pthread_rwlock_t* host_rwlock = FindOrCreateHostRwlock(rwlock, true);
  return host_rwlock == nullptr ? EINVAL : ::pthread_rwlock_wrlock(host_rwlock);
}

int mocktail_bionic_pthread_rwlock_unlock(pthread_rwlock_t* rwlock) {
  pthread_rwlock_t* host_rwlock = FindOrCreateHostRwlock(rwlock, false);
  return host_rwlock == nullptr ? EINVAL : ::pthread_rwlock_unlock(host_rwlock);
}

}  // extern "C"
