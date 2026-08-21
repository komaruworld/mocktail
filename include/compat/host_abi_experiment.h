#ifndef MOCKTAIL_COMPAT_HOST_ABI_EXPERIMENT_H_
#define MOCKTAIL_COMPAT_HOST_ABI_EXPERIMENT_H_

#include <cstddef>
#include <cstdint>

#include "compat/host_abi_profile.h"

namespace mocktail::compat {

struct HostAbiBridgeTargets {
  void *allocate = nullptr;
  void *reallocate = nullptr;
  void *aligned_allocate = nullptr;
  void *free = nullptr;
  void *usable_size = nullptr;
  void *allocator_object_allocate = nullptr;
  void *null_vtable_stub = nullptr;
};

struct HostAbiExperimentOptions {
  bool install_allocator_bridges = true;
  bool seed_allocator_object = true;
  bool seed_empty_string = true;
  bool initialize_arena = true;
  bool seed_jni_singleton = true;
};

struct HostAbiExperimentResult {
  bool installed = false;
  bool uses_native_mimalloc = false;
  size_t bridge_entries_installed = 0;
  const char *error = nullptr;

  explicit operator bool() const noexcept { return installed; }
};

enum class NativeMimallocBootstrapStatus {
  kNotRequired,
  kInitialized,
  kFailed,
};

enum class NativePreJniBootstrapStatus {
  kNotRequired,
  kAlreadyInitialized,
  kInitialized,
  kFailed,
};

// Installs one exact-Build-ID profile before libroblox constructors run.
// The caller owns policy: this function never enables itself from environment
// variables and must only receive a profile selected from the loaded ELF ID.
HostAbiExperimentResult
InstallHostAbiExperiment(uintptr_t library_base, const HostAbiProfile &profile,
                         const HostAbiBridgeTargets &targets,
                         const HostAbiExperimentOptions &options) noexcept;

// Calls the payload's idempotent native allocator TLS initializer on the
// current thread. It must run on each thread that invokes native Roblox code.
bool InitializeHostAbiThread(uintptr_t library_base,
                             const HostAbiProfile &profile) noexcept;

// Applies Build-ID-scoped native mimalloc lifecycle ordering after one
// constructor returns. No payload code is patched: this only invokes the
// profile's existing native thread initializer at its verified boundary.
NativeMimallocBootstrapStatus CompleteNativeMimallocConstructor(
    uintptr_t library_base, const HostAbiProfile &profile,
    size_t constructor_index) noexcept;

// Runs one verified native registry initializer after selected static
// constructors and before JNI_OnLoad. The payload owns both allocation and
// storage; Mocktail only checks the profile-defined postcondition.
NativePreJniBootstrapStatus InitializeNativePreJniRegistry(
    uintptr_t library_base, const HostAbiProfile &profile) noexcept;

} // namespace mocktail::compat

#endif // MOCKTAIL_COMPAT_HOST_ABI_EXPERIMENT_H_
