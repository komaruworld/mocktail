#include "compat/host_abi_experiment.h"

#include <array>

#include <gtest/gtest.h>

namespace mocktail::compat {
namespace {

void **g_test_registry_slot = nullptr;

void InitializeTestRegistry() {
  *g_test_registry_slot = g_test_registry_slot;
}

int g_thread_initializations = 0;

void InitializeTestThread() { ++g_thread_initializations; }

TEST(HostAbiExperimentTest, NativeOnlyProfileRejectsHostActionsBeforeWriting) {
  HostAbiProfile profile =
      *FindHostAbiProfile("d0cb1fa0deb3d9161b4cd77530cbcd2e50de3a21");
  profile.host_allocator_bridges_supported = false;
  const HostAbiBridgeTargets targets;
  const auto rejected = InstallHostAbiExperiment(1, profile, targets, {});
  EXPECT_FALSE(rejected);
  EXPECT_STREQ(rejected.error,
               "this host ABI profile only supports native mimalloc");
  const HostAbiExperimentOptions native{false, false, false, false, false};
  EXPECT_TRUE(InstallHostAbiExperiment(1, profile, targets, native));
}

TEST(HostAbiExperimentTest, InitializesThreadAtRelocatedAllocatorConstructor) {
  HostAbiProfile profile;
  profile.init_array_count = 8;
  profile.native_mimalloc_constructor_run_ranges[0] = {2, 8};
  profile.native_mimalloc_constructor_run_range_count = 1;
  profile.native_mimalloc_thread_initializer_after_constructor = 5;
  profile.data_seeds.allocator_thread_initializer =
      reinterpret_cast<uintptr_t>(&InitializeTestThread) - 1;
  g_thread_initializations = 0;
  for (std::size_t index = 0; index < 8; ++index) {
    EXPECT_EQ(CompleteNativeMimallocConstructor(1, profile, index),
              index == 5 ? NativeMimallocBootstrapStatus::kInitialized
                         : NativeMimallocBootstrapStatus::kNotRequired);
  }
  EXPECT_EQ(g_thread_initializations, 1);
}

TEST(HostAbiExperimentTest, NativeMimallocModeDoesNotInstallHostTrampolines) {
  const HostAbiProfile *profile =
      FindHostAbiProfile("d0cb1fa0deb3d9161b4cd77530cbcd2e50de3a21");
  ASSERT_NE(profile, nullptr);

  const HostAbiBridgeTargets no_host_targets;
  const HostAbiExperimentOptions native_mimalloc_options{false, false, false,
                                                         false, false};
  const HostAbiExperimentResult result = InstallHostAbiExperiment(
      1, *profile, no_host_targets, native_mimalloc_options);

  EXPECT_TRUE(result);
  EXPECT_TRUE(result.uses_native_mimalloc);
  EXPECT_EQ(result.bridge_entries_installed, 0U);
}

TEST(HostAbiExperimentTest, NativePreJniRegistryUsesPayloadInitializerOnce) {
  std::array<void *, 2> slots{};
  g_test_registry_slot = &slots[1];

  HostAbiProfile profile;
  const uintptr_t base = reinterpret_cast<uintptr_t>(slots.data());
  profile.native_pre_jni_bootstrap = {
      reinterpret_cast<uintptr_t>(&InitializeTestRegistry) - base,
      sizeof(void *)};

  EXPECT_EQ(InitializeNativePreJniRegistry(base, profile),
            NativePreJniBootstrapStatus::kInitialized);
  EXPECT_EQ(slots[1], static_cast<void *>(&slots[1]));
  EXPECT_EQ(InitializeNativePreJniRegistry(base, profile),
            NativePreJniBootstrapStatus::kAlreadyInitialized);
}

} // namespace
} // namespace mocktail::compat
