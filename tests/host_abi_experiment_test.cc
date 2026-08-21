#include "compat/host_abi_experiment.h"

#include <array>

#include <gtest/gtest.h>

namespace mocktail::compat {
namespace {

void **g_test_registry_slot = nullptr;

void InitializeTestRegistry() {
  *g_test_registry_slot = g_test_registry_slot;
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
