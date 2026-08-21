#ifndef MOCKTAIL_COMPAT_BIONIC_RWLOCK_RUNTIME_H_
#define MOCKTAIL_COMPAT_BIONIC_RWLOCK_RUNTIME_H_

#include <pthread.h>

// Bionic and glibc pthread_rwlock_t storage is not ABI-compatible. The guest
// address is an opaque key; the host object lives entirely in this runtime.
extern "C" {

int mocktail_bionic_pthread_rwlock_init(pthread_rwlock_t* rwlock,
                                        const pthread_rwlockattr_t* attr);
int mocktail_bionic_pthread_rwlock_destroy(pthread_rwlock_t* rwlock);
int mocktail_bionic_pthread_rwlock_rdlock(pthread_rwlock_t* rwlock);
int mocktail_bionic_pthread_rwlock_wrlock(pthread_rwlock_t* rwlock);
int mocktail_bionic_pthread_rwlock_unlock(pthread_rwlock_t* rwlock);

}  // extern "C"

#endif  // MOCKTAIL_COMPAT_BIONIC_RWLOCK_RUNTIME_H_
