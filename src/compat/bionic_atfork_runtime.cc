#include "compat/bionic_atfork_runtime.h"

#include <pthread.h>

namespace mocktail::compat {

int RegisterBionicAtFork(BionicAtForkCallback prepare,
                         BionicAtForkCallback parent,
                         BionicAtForkCallback child,
                         void* dso_handle) noexcept {
  // pthread_atfork preserves the Android/POSIX callback order and is provided
  // by both glibc and musl. Its public contract intentionally has no DSO
  // lifetime hook; see the constraint documented in the public header.
  static_cast<void>(dso_handle);
  return pthread_atfork(prepare, parent, child);
}

}  // namespace mocktail::compat

extern "C" int
mocktail_bionic_register_atfork(mocktail::compat::BionicAtForkCallback prepare,
                                mocktail::compat::BionicAtForkCallback parent,
                                mocktail::compat::BionicAtForkCallback child,
                                void* dso_handle) {
  return mocktail::compat::RegisterBionicAtFork(prepare, parent, child,
                                                dso_handle);
}
