#ifndef MOCKTAIL_COMPAT_BIONIC_SEMAPHORE_RUNTIME_H_
#define MOCKTAIL_COMPAT_BIONIC_SEMAPHORE_RUNTIME_H_

#include <semaphore.h>

// Bionic and glibc use different sem_t representations. These exports keep
// host semaphore objects out of guest storage and key them by guest address.
extern "C" {

int mocktail_bionic_sem_init(sem_t* semaphore, int process_shared,
                             unsigned int value);
int mocktail_bionic_sem_destroy(sem_t* semaphore);
int mocktail_bionic_sem_wait(sem_t* semaphore);
int mocktail_bionic_sem_post(sem_t* semaphore);

}  // extern "C"

#endif  // MOCKTAIL_COMPAT_BIONIC_SEMAPHORE_RUNTIME_H_
