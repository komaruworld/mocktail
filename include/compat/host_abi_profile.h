#ifndef MOCKTAIL_COMPAT_HOST_ABI_PROFILE_H_
#define MOCKTAIL_COMPAT_HOST_ABI_PROFILE_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace mocktail::compat {

enum class HostBridgeKind {
  kAllocate,
  kReallocate,
  kAlignedAllocate,
  kFree,
  kUsableSize,
};

enum class HostAllocatorStrategy {
  kHostBridges,
  kNativeMimalloc,
};

struct HostBridgeEntry {
  uintptr_t rva = 0;
  HostBridgeKind kind = HostBridgeKind::kAllocate;
  std::string_view label;
};

struct ConstructorRange {
  size_t begin = 0;
  size_t end_exclusive = 0;

  bool IsValidFor(size_t constructor_count) const noexcept {
    return begin < end_exclusive && end_exclusive <= constructor_count;
  }
};

struct HostDataSeedProfile {
  uintptr_t allocator_object_slot = 0;
  uintptr_t empty_string_slot = 0;
  uintptr_t jni_singleton_slot = 0;
  size_t jni_singleton_bytes = 0;
  uintptr_t arena_initializer = 0;
  uintptr_t allocator_thread_initializer = 0;
  uintptr_t arena_guard_slot = 0;
  uintptr_t arena_table_slot = 0;
  size_t arena_table_slot_count = 0;
};

struct NativePreJniBootstrapProfile {
  uintptr_t registry_initializer = 0;
  uintptr_t registry_slot = 0;

  bool IsValid() const noexcept {
    return registry_initializer != 0 && registry_slot != 0;
  }
};

struct NativeAllocatorProfile {
  uintptr_t allocate = 0;
  uintptr_t deallocate = 0;

  bool IsValid() const noexcept { return allocate != 0 && deallocate != 0; }
};

inline constexpr size_t kMaxHostBridgeEntries = 8;
inline constexpr size_t kMaxConstructorRunRanges = 26;

// Build-ID-scoped native ABI knowledge for supported and candidate payload
// profiles. Keeping these addresses outside the legacy startup graph makes the
// version boundary reviewable and independently testable.
struct HostAbiProfile {
  std::string_view elf_build_id;
  std::array<HostBridgeEntry, kMaxHostBridgeEntries> bridge_entries{};
  size_t bridge_entry_count = 0;
  HostDataSeedProfile data_seeds;
  NativeAllocatorProfile native_allocator;
  uintptr_t init_array_offset = 0;
  size_t init_array_count = 0;
  std::array<ConstructorRange, kMaxConstructorRunRanges>
      constructor_run_ranges{};
  size_t constructor_run_range_count = 0;
  // Verified native-mimalloc-compatible replay that keeps the payload's
  // dependency-ordered pre-JNI static state coherent. This is distinct from
  // the host bridge bring-up path and must not use host malloc trampolines.
  std::array<ConstructorRange, kMaxConstructorRunRanges>
      native_mimalloc_constructor_run_ranges{};
  size_t native_mimalloc_constructor_run_range_count = 0;
  // The native process-load constructor has completed at this index. The
  // per-thread initializer must run before the following integration probe.
  size_t native_mimalloc_thread_initializer_after_constructor = 0;
  // Exact payload initializer that constructs the native registry needed by
  // JNI callbacks registered during the selected constructor replay.
  NativePreJniBootstrapProfile native_pre_jni_bootstrap;
  // Strategy used when MOCKTAIL_HOST_ALLOCATOR_BRIDGES has no explicit
  // override. New verified payloads retain their native allocator; researched
  // legacy profiles keep the historical host-bridge default.
  HostAllocatorStrategy default_allocator_strategy =
      HostAllocatorStrategy::kHostBridges;

  bool HasValidConstructorRanges() const noexcept;
  bool AllowsConstructor(size_t index) const noexcept;
  size_t ConstructorRangeBegin() const noexcept;
  size_t ConstructorRangeEndExclusive() const noexcept;
  bool HasValidNativeMimallocConstructorRanges() const noexcept;
  bool AllowsNativeMimallocConstructor(size_t index) const noexcept;
  size_t NativeMimallocConstructorRangeBegin() const noexcept;
  size_t NativeMimallocConstructorRangeEndExclusive() const noexcept;
  bool ShouldInitializeNativeMimallocThreadAfterConstructor(
      size_t index) const noexcept;
  bool HasValidNativePreJniBootstrap() const noexcept;
  HostAllocatorStrategy ResolveAllocatorStrategy(
      bool host_bridges_override_present,
      bool host_bridges_override_enabled) const noexcept;
};

const HostAbiProfile *FindHostAbiProfile(std::string_view build_id) noexcept;

} // namespace mocktail::compat

#endif // MOCKTAIL_COMPAT_HOST_ABI_PROFILE_H_
