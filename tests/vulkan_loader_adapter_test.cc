#include <gtest/gtest.h>
#include <vulkan/vulkan.h>

#include <dlfcn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>

namespace {

void ExpectAdapterDispatch(bool host_first) {
  // Isolate the ELF global symbol scope for each load order. A dependency
  // such as libplacebo can load the system Vulkan library before our adapter.
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    void* host = nullptr;
    if (host_first) {
      host = dlopen(MOCKTAIL_VULKAN_HOST_FIXTURE, RTLD_NOW | RTLD_GLOBAL);
      if (host == nullptr) std::_Exit(10);
    }
    void* adapter = dlopen(MOCKTAIL_VULKAN_ADAPTER, RTLD_NOW | RTLD_LOCAL);
    if (adapter == nullptr) {
      std::fprintf(stderr, "Vulkan adapter load failed: %s\n", dlerror());
      std::_Exit(11);
    }
    if (!host_first) {
      host = dlopen(MOCKTAIL_VULKAN_HOST_FIXTURE, RTLD_NOW | RTLD_GLOBAL);
      if (host == nullptr) std::_Exit(12);
    }
    const auto get = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        dlsym(adapter, "vkGetInstanceProcAddr"));
    if (get == nullptr) std::_Exit(13);

    for (const char* name : {"vkCreateInstance", "vkGetInstanceProcAddr",
                             "vkEnumerateInstanceExtensionProperties",
                             "vkEnumerateInstanceLayerProperties"}) {
      const auto expected =
          reinterpret_cast<PFN_vkVoidFunction>(dlsym(adapter, name));
      const auto host_function =
          reinterpret_cast<PFN_vkVoidFunction>(dlsym(host, name));
      if (expected == nullptr || expected == host_function ||
          get(VK_NULL_HANDLE, name) != expected) {
        std::fprintf(stderr, "Vulkan adapter dispatch escaped to host: %s\n",
                     name);
        std::_Exit(14);
      }
    }
    std::_Exit(0);
  }
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST(VulkanLoaderAdapterTest, KeepsOwnDispatchWhenHostLoadsFirst) {
  ExpectAdapterDispatch(true);
}

TEST(VulkanLoaderAdapterTest, KeepsOwnDispatchWhenAdapterLoadsFirst) {
  ExpectAdapterDispatch(false);
}

}  // namespace
