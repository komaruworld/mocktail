#include "compat/host_abi_profile.h"

#include "compat/host_abi_profile_loader.h"

namespace mocktail::compat {
namespace {

constexpr HostAbiProfile kProfiles[] = {
    // Roblox 2.734.917. This exact Build-ID profile was machine-derived from
    // the verified 2.727.1199 reference and passed an isolated Tier C Vulkan
    // probation canary with native constructors, JNI startup, presentation,
    // and controlled lifecycle teardown.
    {"63c5109637b7d7b2bdb8ed8f858023ff5ef49326",
     {{{0x1cf69e2, HostBridgeKind::kAllocate, "small-allocate"},
       {0x1cf9c3a, HostBridgeKind::kUsableSize, "usable-size"},
       {0x21af33f, HostBridgeKind::kReallocate, "reallocate"},
       {0x1cf677e, HostBridgeKind::kAllocate, "allocate"},
       {0x1d12197, HostBridgeKind::kAlignedAllocate,
        "aligned-allocate-direct"},
       {0x1cfa1d9, HostBridgeKind::kFree, "free"}}},
     6,
     {0x7571570, 0x767e428, 0x7ad47e8, 0x400, 0x1cf6df8, 0x1d82be7,
      0x7a10730, 0x7a107c0, 0x400000},
     {0x1cf677e, 0x1cfa1d9},
     0x6f78a18,
     3556,
     {{{2, 3}, {5, 3556}}},
     2,
     {{{2, 3556}}},
     1,
     2,
     {0x21b1889, 0x7a14898},
     HostAllocatorStrategy::kNativeMimalloc},
    // Roblox 2.727.1199. Every function and state slot below was matched
    // independently against the previous supported payload by normalized
    // instruction signatures and RIP-relative references. The payload keeps
    // the same native mimalloc/bootstrap design, but its .init_array has 3481
    // entries and all addresses are scoped to this exact ELF Build ID.
    {"1686400865ae0e408cd7bd67de7a439625c6fd13",
     {{{0x1c39ea2, HostBridgeKind::kAllocate, "small-allocate"},
       {0x1fe4c84, HostBridgeKind::kUsableSize, "usable-size"},
       {0x203daae, HostBridgeKind::kReallocate, "reallocate"},
       {0x1c39c3a, HostBridgeKind::kAllocate, "allocate"},
       {0x1c8dbf8, HostBridgeKind::kAlignedAllocate,
        "aligned-allocate-direct"},
       {0x1c3eb72, HostBridgeKind::kFree, "free"}}},
     6,
     {0x7030ae0, 0x703a7e8, 0x72a16d0, 0x400, 0x1c39f1d, 0x2040817,
      0x71e92f0, 0x71e9380, 0x400000},
     {0x1c39c3a, 0x1c3eb72},
     0x6c2c9b0,
     0x6cc8 / 8,
     {{{2, 3}, {5, 3481}}},
     2,
     {{{2, 3481}}},
     1,
     2,
     {0x1fe72e5, 0x71ed428},
     HostAllocatorStrategy::kNativeMimalloc},
    // Roblox 2.725.1142. The range comes from its relocated .init_array.
    // Constructors 2..3454 have been replayed in original order with the
    // payload's native mimalloc and completed cleanly in no-recovery Tier A
    // and Tier C runs. Keeping the complete verified prefix is upstream-first:
    // it preserves dependency-ordered guest initialization instead of growing
    // a hand-maintained allowlist whenever another native singleton is used.
    // Entries 0 and 1 remain excluded. The tail at 3443..3454 installs JNI
    // registration and native runtime tables consumed by JNI_OnLoad; omitting
    // it leaves later graphics/shader state only partially initialized.
    // The secondary aligned allocator is called directly by LuaApp startup and
    // therefore cannot be covered by the public wrapper entry alone.
    {"d0cb1fa0deb3d9161b4cd77530cbcd2e50de3a21",
     {{{0x1bf2de2, HostBridgeKind::kAllocate, "small-allocate"},
       {0x1f78ad4, HostBridgeKind::kUsableSize, "usable-size"},
       {0x1fd3261, HostBridgeKind::kReallocate, "reallocate"},
       {0x1bf2b81, HostBridgeKind::kAllocate, "allocate"},
       {0x1c45f5b, HostBridgeKind::kAlignedAllocate, "aligned-allocate-direct"},
       {0x1bf7aba, HostBridgeKind::kFree, "free"}}},
     6,
     {0x6f0cec0, 0x6f15978, 0x717b680, 0x400, 0x1bf2e5d, 0x1fd61f3, 0x70c3730,
      0x70c37c0, 0x400000},
     {0x1bf2b81, 0x1bf7aba},
     0x6b16c40,
     0x6bf8 / 8,
     {{{2, 3}, {5, 3455}}},
     2,
     {{{2, 3455}}},
     1,
     2,
     {0x1f7b131, 0x70c7868},
     HostAllocatorStrategy::kNativeMimalloc},
    // Roblox 2.721.1108 researched baseline.
    {"50e1b0abd123350e794226062fe3a1ef360c5f0d",
     {{{0x1f24322, HostBridgeKind::kAllocate, "small-allocate"}}},
     1,
     {},
     {},
     0,
     0,
     {},
     0,
     {},
     0,
     0,
     {}},
};

} // namespace

const HostAbiProfile *FindHostAbiProfile(std::string_view build_id) noexcept {
  for (const HostAbiProfile &profile : kProfiles) {
    if (profile.elf_build_id == build_id) {
      return &profile;
    }
  }
  return FindLoadedExternalHostAbiProfile(build_id);
}

bool HostAbiProfile::HasValidConstructorRanges() const noexcept {
  if (constructor_run_range_count == 0 ||
      constructor_run_range_count > constructor_run_ranges.size()) {
    return false;
  }
  size_t previous_end = 0;
  for (size_t index = 0; index < constructor_run_range_count; ++index) {
    const ConstructorRange &range = constructor_run_ranges[index];
    if (!range.IsValidFor(init_array_count) ||
        (index != 0 && range.begin < previous_end)) {
      return false;
    }
    previous_end = range.end_exclusive;
  }
  return true;
}

bool HostAbiProfile::AllowsConstructor(size_t index) const noexcept {
  if (!HasValidConstructorRanges()) {
    return false;
  }
  for (size_t range_index = 0; range_index < constructor_run_range_count;
       ++range_index) {
    const ConstructorRange &range = constructor_run_ranges[range_index];
    if (range.begin <= index && index < range.end_exclusive) {
      return true;
    }
  }
  return false;
}

size_t HostAbiProfile::ConstructorRangeBegin() const noexcept {
  return HasValidConstructorRanges() ? constructor_run_ranges[0].begin : 0;
}

size_t HostAbiProfile::ConstructorRangeEndExclusive() const noexcept {
  return HasValidConstructorRanges()
             ? constructor_run_ranges[constructor_run_range_count - 1]
                   .end_exclusive
             : 0;
}

bool HostAbiProfile::HasValidNativeMimallocConstructorRanges() const noexcept {
  if (native_mimalloc_constructor_run_range_count == 0 ||
      native_mimalloc_constructor_run_range_count >
          native_mimalloc_constructor_run_ranges.size()) {
    return false;
  }
  size_t previous_end = 0;
  for (size_t index = 0; index < native_mimalloc_constructor_run_range_count;
       ++index) {
    const ConstructorRange &range =
        native_mimalloc_constructor_run_ranges[index];
    if (!range.IsValidFor(init_array_count) ||
        (index != 0 && range.begin < previous_end)) {
      return false;
    }
    previous_end = range.end_exclusive;
  }
  return true;
}

bool HostAbiProfile::AllowsNativeMimallocConstructor(
    size_t index) const noexcept {
  if (!HasValidNativeMimallocConstructorRanges()) {
    return false;
  }
  for (size_t range_index = 0;
       range_index < native_mimalloc_constructor_run_range_count;
       ++range_index) {
    const ConstructorRange &range =
        native_mimalloc_constructor_run_ranges[range_index];
    if (range.begin <= index && index < range.end_exclusive) {
      return true;
    }
  }
  return false;
}

size_t HostAbiProfile::NativeMimallocConstructorRangeBegin() const noexcept {
  return HasValidNativeMimallocConstructorRanges()
             ? native_mimalloc_constructor_run_ranges[0].begin
             : 0;
}

size_t
HostAbiProfile::NativeMimallocConstructorRangeEndExclusive() const noexcept {
  return HasValidNativeMimallocConstructorRanges()
             ? native_mimalloc_constructor_run_ranges
                   [native_mimalloc_constructor_run_range_count - 1]
                       .end_exclusive
             : 0;
}

bool HostAbiProfile::ShouldInitializeNativeMimallocThreadAfterConstructor(
    size_t index) const noexcept {
  return HasValidNativeMimallocConstructorRanges() &&
         native_mimalloc_thread_initializer_after_constructor <
             init_array_count &&
         AllowsNativeMimallocConstructor(
             native_mimalloc_thread_initializer_after_constructor) &&
         index == native_mimalloc_thread_initializer_after_constructor;
}

bool HostAbiProfile::HasValidNativePreJniBootstrap() const noexcept {
  return native_pre_jni_bootstrap.IsValid();
}

HostAllocatorStrategy HostAbiProfile::ResolveAllocatorStrategy(
    bool host_bridges_override_present,
    bool host_bridges_override_enabled) const noexcept {
  if (!host_bridges_override_present) {
    return default_allocator_strategy;
  }
  return host_bridges_override_enabled
             ? HostAllocatorStrategy::kHostBridges
             : HostAllocatorStrategy::kNativeMimalloc;
}

} // namespace mocktail::compat
