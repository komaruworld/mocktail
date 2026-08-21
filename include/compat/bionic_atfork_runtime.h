#ifndef MOCKTAIL_COMPAT_BIONIC_ATFORK_RUNTIME_H_
#define MOCKTAIL_COMPAT_BIONIC_ATFORK_RUNTIME_H_

namespace mocktail::compat {

using BionicAtForkCallback = void (*)(void);

// POSIX cannot unregister by dso_handle, so registered guest callbacks must
// remain mapped for every later fork.
int RegisterBionicAtFork(BionicAtForkCallback prepare,
                         BionicAtForkCallback parent,
                         BionicAtForkCallback child, void* dso_handle) noexcept;

}  // namespace mocktail::compat

extern "C" {

int mocktail_bionic_register_atfork(
    mocktail::compat::BionicAtForkCallback prepare,
    mocktail::compat::BionicAtForkCallback parent,
    mocktail::compat::BionicAtForkCallback child, void* dso_handle);

}  // extern "C"

#endif  // MOCKTAIL_COMPAT_BIONIC_ATFORK_RUNTIME_H_
