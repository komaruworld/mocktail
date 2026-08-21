#include "compat/host_abi_experiment.h"

#include <sys/mman.h>
#include <unistd.h>

#include <array>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>

namespace mocktail::compat {
namespace {

std::array<uintptr_t, 4> g_allocator_object_vtable{};
std::array<uintptr_t, 1> g_allocator_object{};
std::array<unsigned char, 32> g_empty_string_object{};

void *TargetForKind(HostBridgeKind kind,
                    const HostAbiBridgeTargets &targets) noexcept {
  switch (kind) {
  case HostBridgeKind::kAllocate:
    return targets.allocate;
  case HostBridgeKind::kReallocate:
    return targets.reallocate;
  case HostBridgeKind::kAlignedAllocate:
    return targets.aligned_allocate;
  case HostBridgeKind::kFree:
    return targets.free;
  case HostBridgeKind::kUsableSize:
    return targets.usable_size;
  }
  return nullptr;
}

bool PatchAbsoluteJump(uintptr_t address, void *target) noexcept {
  const long page_size = sysconf(_SC_PAGESIZE);
  if (page_size <= 0 || address == 0 || target == nullptr) {
    return false;
  }

  unsigned char patch[] = {
      0x48, 0xb8,                               // movabs rax, imm64
      0,    0,    0, 0, 0, 0, 0, 0, 0xff, 0xe0, // jmp rax
  };
  const uintptr_t target_address = reinterpret_cast<uintptr_t>(target);
  std::memcpy(patch + 2, &target_address, sizeof(target_address));

  const uintptr_t page_mask = static_cast<uintptr_t>(page_size) - 1;
  const uintptr_t page = address & ~page_mask;
  const uintptr_t end = address + sizeof(patch);
  const uintptr_t page_end = (end + page_mask) & ~page_mask;
  const size_t protect_size = page_end - page;
  if (mprotect(reinterpret_cast<void *>(page), protect_size,
               PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
    return false;
  }
  std::memcpy(reinterpret_cast<void *>(address), patch, sizeof(patch));
  __builtin___clear_cache(reinterpret_cast<char *>(address),
                          reinterpret_cast<char *>(address) + sizeof(patch));
  return mprotect(reinterpret_cast<void *>(page), protect_size,
                  PROT_READ | PROT_EXEC) == 0;
}

bool SeedAllocatorObject(uintptr_t library_base,
                         const HostDataSeedProfile &seeds,
                         const HostAbiBridgeTargets &targets) noexcept {
  if (seeds.allocator_object_slot == 0) {
    return true;
  }
  if (targets.allocator_object_allocate == nullptr ||
      targets.null_vtable_stub == nullptr) {
    return false;
  }

  g_allocator_object_vtable.fill(
      reinterpret_cast<uintptr_t>(targets.null_vtable_stub));
  g_allocator_object_vtable[2] =
      reinterpret_cast<uintptr_t>(targets.allocator_object_allocate);
  g_allocator_object[0] =
      reinterpret_cast<uintptr_t>(g_allocator_object_vtable.data());

  auto **slot =
      reinterpret_cast<void **>(library_base + seeds.allocator_object_slot);
  *slot = g_allocator_object.data();
  std::cout << "  [compat] seeded allocator object @+0x" << std::hex
            << seeds.allocator_object_slot << std::dec << " ptr=" << *slot
            << '\n';
  return true;
}

void SeedEmptyString(uintptr_t library_base,
                     const HostDataSeedProfile &seeds) noexcept {
  if (seeds.empty_string_slot == 0) {
    return;
  }
  auto **slot =
      reinterpret_cast<void **>(library_base + seeds.empty_string_slot);
  if (*slot == nullptr) {
    *slot = g_empty_string_object.data();
    std::cout << "  [compat] seeded empty-string object @+0x" << std::hex
              << seeds.empty_string_slot << std::dec << " ptr=" << *slot
              << '\n';
  }
}

bool InitializeArena(uintptr_t library_base,
                     const HostDataSeedProfile &seeds) noexcept {
  if (seeds.arena_initializer == 0) {
    return true;
  }
  if (seeds.arena_guard_slot == 0 || seeds.arena_table_slot == 0 ||
      seeds.arena_table_slot_count == 0) {
    return false;
  }

  using ArenaInitializer = void (*)();
  auto *initializer = reinterpret_cast<ArenaInitializer>(
      library_base + seeds.arena_initializer);
  std::cout << "  [compat] calling host allocator arena init at +0x" << std::hex
            << seeds.arena_initializer << std::dec << '\n';
  initializer();

  auto **guard_slot =
      reinterpret_cast<void **>(library_base + seeds.arena_guard_slot);
  auto **table_slot =
      reinterpret_cast<void **>(library_base + seeds.arena_table_slot);
  std::cout << "  [compat] host allocator arena init returned guard="
            << *guard_slot << " table=" << *table_slot << '\n';
  if (*table_slot != nullptr) {
    return true;
  }

  void **table = static_cast<void **>(
      std::calloc(seeds.arena_table_slot_count, sizeof(void *)));
  if (table == nullptr) {
    return false;
  }
  *table_slot = table;
  if (*guard_slot == nullptr) {
    *guard_slot = reinterpret_cast<void *>(1);
  }
  std::cout << "  [compat] seeded empty host arena table slots=0x" << std::hex
            << seeds.arena_table_slot_count << std::dec << " ptr=" << table
            << '\n';
  return true;
}

bool SeedJniSingleton(uintptr_t library_base,
                      const HostDataSeedProfile &seeds) noexcept {
  if (seeds.jni_singleton_slot == 0 || seeds.jni_singleton_bytes == 0) {
    return true;
  }
  auto **slot =
      reinterpret_cast<void **>(library_base + seeds.jni_singleton_slot);
  if (*slot != nullptr) {
    return true;
  }

  auto *object =
      static_cast<unsigned char *>(std::calloc(1, seeds.jni_singleton_bytes));
  if (object == nullptr) {
    return false;
  }
  constexpr size_t kMapOffset = 0x30;
  constexpr size_t kLoadFactorOffset = kMapOffset + 0x20;
  if (seeds.jni_singleton_bytes >= kLoadFactorOffset + sizeof(float)) {
    const float load_factor = 1.0F;
    std::memcpy(object + kLoadFactorOffset, &load_factor, sizeof(load_factor));
  }
  *slot = object;
  std::cout << "  [compat] seeded JNI singleton @+0x" << std::hex
            << seeds.jni_singleton_slot << std::dec
            << " bytes=" << seeds.jni_singleton_bytes << " ptr=" << object
            << '\n';
  return true;
}

} // namespace

HostAbiExperimentResult
InstallHostAbiExperiment(uintptr_t library_base, const HostAbiProfile &profile,
                         const HostAbiBridgeTargets &targets,
                         const HostAbiExperimentOptions &options) noexcept {
  HostAbiExperimentResult result;
  result.uses_native_mimalloc = !options.install_allocator_bridges;
  if (library_base == 0 || profile.bridge_entry_count == 0 ||
      profile.bridge_entry_count > profile.bridge_entries.size()) {
    result.error = "invalid host ABI profile";
    return result;
  }

  bool ok = true;
  if (options.install_allocator_bridges) {
    for (size_t index = 0; index < profile.bridge_entry_count; ++index) {
      const HostBridgeEntry &entry = profile.bridge_entries[index];
      void *target = TargetForKind(entry.kind, targets);
      const bool installed =
          entry.rva != 0 && PatchAbsoluteJump(library_base + entry.rva, target);
      std::cout << "  [compat] host " << entry.label << " bridge at +0x"
                << std::hex << entry.rva << std::dec
                << (installed ? " installed" : " failed") << '\n';
      if (installed) {
        ++result.bridge_entries_installed;
      }
      ok = installed && ok;
    }
  } else {
    std::cout << "  [compat] native allocator retained; host allocator "
                 "bridges disabled\n";
  }

  if (options.seed_allocator_object) {
    ok = SeedAllocatorObject(library_base, profile.data_seeds, targets) && ok;
  }
  if (options.seed_empty_string) {
    SeedEmptyString(library_base, profile.data_seeds);
  }
  if (options.initialize_arena) {
    ok = InitializeArena(library_base, profile.data_seeds) && ok;
  }
  if (options.seed_jni_singleton) {
    ok = SeedJniSingleton(library_base, profile.data_seeds) && ok;
  }

  std::cout << std::flush;
  result.installed = ok;
  if (!ok) {
    result.error = "one or more host ABI actions failed";
  }
  return result;
}

bool InitializeHostAbiThread(uintptr_t library_base,
                             const HostAbiProfile &profile) noexcept {
  if (library_base == 0 ||
      profile.data_seeds.allocator_thread_initializer == 0) {
    return false;
  }
  using ThreadInitializer = void (*)();
  auto *initializer = reinterpret_cast<ThreadInitializer>(
      library_base + profile.data_seeds.allocator_thread_initializer);
  initializer();
  return true;
}

NativeMimallocBootstrapStatus CompleteNativeMimallocConstructor(
    uintptr_t library_base, const HostAbiProfile &profile,
    size_t constructor_index) noexcept {
  if (!profile.ShouldInitializeNativeMimallocThreadAfterConstructor(
          constructor_index)) {
    return NativeMimallocBootstrapStatus::kNotRequired;
  }
  return InitializeHostAbiThread(library_base, profile)
             ? NativeMimallocBootstrapStatus::kInitialized
             : NativeMimallocBootstrapStatus::kFailed;
}

NativePreJniBootstrapStatus InitializeNativePreJniRegistry(
    uintptr_t library_base, const HostAbiProfile &profile) noexcept {
  if (library_base == 0 || !profile.HasValidNativePreJniBootstrap()) {
    return NativePreJniBootstrapStatus::kNotRequired;
  }

  auto **registry_slot = reinterpret_cast<void **>(
      library_base + profile.native_pre_jni_bootstrap.registry_slot);
  if (*registry_slot != nullptr) {
    return NativePreJniBootstrapStatus::kAlreadyInitialized;
  }

  using RegistryInitializer = void (*)();
  auto *initializer = reinterpret_cast<RegistryInitializer>(
      library_base + profile.native_pre_jni_bootstrap.registry_initializer);
  initializer();
  return *registry_slot != nullptr ? NativePreJniBootstrapStatus::kInitialized
                                   : NativePreJniBootstrapStatus::kFailed;
}

} // namespace mocktail::compat
