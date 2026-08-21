#include "compat/bionic_semaphore_runtime.h"

#include <errno.h>
#include <pthread.h>

#include <cstdint>
#include <cstdlib>

namespace {

struct HostSemaphoreEntry {
  uintptr_t guest_address;
  sem_t host_semaphore;
  HostSemaphoreEntry* next;
};

pthread_mutex_t g_table_mutex = PTHREAD_MUTEX_INITIALIZER;
HostSemaphoreEntry* g_table = nullptr;

sem_t* FindOrCreateHostSemaphore(sem_t* guest_semaphore, bool create,
                                 unsigned int initial_value) noexcept {
  if (guest_semaphore == nullptr) {
    return nullptr;
  }

  const uintptr_t guest_address =
      reinterpret_cast<uintptr_t>(guest_semaphore);
  ::pthread_mutex_lock(&g_table_mutex);
  for (HostSemaphoreEntry* entry = g_table; entry != nullptr;
       entry = entry->next) {
    if (entry->guest_address == guest_address) {
      ::pthread_mutex_unlock(&g_table_mutex);
      return &entry->host_semaphore;
    }
  }

  if (!create) {
    ::pthread_mutex_unlock(&g_table_mutex);
    return nullptr;
  }

  auto* entry = static_cast<HostSemaphoreEntry*>(
      std::calloc(1, sizeof(HostSemaphoreEntry)));
  if (entry == nullptr) {
    ::pthread_mutex_unlock(&g_table_mutex);
    errno = ENOMEM;
    return nullptr;
  }
  if (::sem_init(&entry->host_semaphore, 0, initial_value) != 0) {
    std::free(entry);
    ::pthread_mutex_unlock(&g_table_mutex);
    return nullptr;
  }

  entry->guest_address = guest_address;
  entry->next = g_table;
  g_table = entry;
  ::pthread_mutex_unlock(&g_table_mutex);
  return &entry->host_semaphore;
}

}  // namespace

extern "C" {

int mocktail_bionic_sem_init(sem_t* semaphore, int process_shared,
                             unsigned int value) {
  if (semaphore == nullptr || process_shared != 0) {
    errno = EINVAL;
    return -1;
  }
  return FindOrCreateHostSemaphore(semaphore, true, value) != nullptr ? 0 : -1;
}

int mocktail_bionic_sem_destroy(sem_t* semaphore) {
  if (semaphore == nullptr) {
    errno = EINVAL;
    return -1;
  }

  const uintptr_t guest_address = reinterpret_cast<uintptr_t>(semaphore);
  ::pthread_mutex_lock(&g_table_mutex);
  HostSemaphoreEntry** link = &g_table;
  while (*link != nullptr) {
    HostSemaphoreEntry* entry = *link;
    if (entry->guest_address == guest_address) {
      *link = entry->next;
      ::pthread_mutex_unlock(&g_table_mutex);
      const int result = ::sem_destroy(&entry->host_semaphore);
      std::free(entry);
      return result;
    }
    link = &entry->next;
  }
  ::pthread_mutex_unlock(&g_table_mutex);
  return 0;
}

int mocktail_bionic_sem_wait(sem_t* semaphore) {
  sem_t* host_semaphore = FindOrCreateHostSemaphore(semaphore, false, 0);
  if (host_semaphore == nullptr) {
    errno = EINVAL;
    return -1;
  }

  int result = 0;
  do {
    result = ::sem_wait(host_semaphore);
  } while (result != 0 && errno == EINTR);
  return result;
}

int mocktail_bionic_sem_post(sem_t* semaphore) {
  sem_t* host_semaphore = FindOrCreateHostSemaphore(semaphore, false, 0);
  if (host_semaphore == nullptr) {
    errno = EINVAL;
    return -1;
  }
  return ::sem_post(host_semaphore);
}

}  // extern "C"
