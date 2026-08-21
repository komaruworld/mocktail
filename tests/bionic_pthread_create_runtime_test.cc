#include "compat/bionic_pthread_create_runtime.h"

#include <errno.h>
#include <gtest/gtest.h>
#include <sched.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>

namespace {

std::atomic<bool> g_intercept_pthread_create{false};
std::atomic<int> g_intercepted_create_calls{0};
std::atomic<int> g_forced_create_failures{0};
std::atomic<size_t> g_first_create_stack_size{0};
std::atomic<size_t> g_second_create_stack_size{0};
std::atomic<size_t> g_third_create_stack_size{0};

size_t ReadRequestedStackSize(const pthread_attr_t* attributes) {
  if (attributes == nullptr) {
    return 0;
  }
  size_t stack_size = 0;
  return pthread_attr_getstacksize(attributes, &stack_size) == 0 ? stack_size
                                                                 : 0;
}

}  // namespace

extern "C" int __real_pthread_create(pthread_t* thread,
                                     const pthread_attr_t* attributes,
                                     void* (*start_routine)(void*),
                                     void* argument);

extern "C" int __wrap_pthread_create(pthread_t* thread,
                                     const pthread_attr_t* attributes,
                                     void* (*start_routine)(void*),
                                     void* argument) {
  if (g_intercept_pthread_create.load(std::memory_order_acquire)) {
    const int call =
        g_intercepted_create_calls.fetch_add(1, std::memory_order_acq_rel);
    const size_t stack_size = ReadRequestedStackSize(attributes);
    if (call == 0) {
      g_first_create_stack_size.store(stack_size, std::memory_order_release);
    } else if (call == 1) {
      g_second_create_stack_size.store(stack_size, std::memory_order_release);
    } else if (call == 2) {
      g_third_create_stack_size.store(stack_size, std::memory_order_release);
    }
    if (call < g_forced_create_failures.load(std::memory_order_acquire)) {
      return EINVAL;
    }
  }
  return __real_pthread_create(thread, attributes, start_routine, argument);
}

namespace mocktail::compat {
namespace {

std::atomic<int> g_thread_phase{0};
std::atomic<size_t> g_observed_stack_size{0};

void ExpectRequestedStackSize(size_t observed, size_t requested) {
#if defined(__GLIBC__)
  // glibc reports exactly the value supplied through pthread_attr_setstacksize.
  EXPECT_EQ(observed, requested);
#else
  // musl's pthread_getattr_np reports its TLS reservation together with the
  // requested stack mapping. The guest stack itself is never smaller than the
  // requested value, and the host-only addition remains below one page.
  const long page_size = sysconf(_SC_PAGESIZE);
  ASSERT_GT(page_size, 0);
  EXPECT_GE(observed, requested);
  EXPECT_LT(observed, requested + static_cast<size_t>(page_size));
#endif
}

void InitializeThread() { g_thread_phase.store(1, std::memory_order_release); }

void* CheckInitializedThread(void*) {
  const int phase = g_thread_phase.load(std::memory_order_acquire);
  g_thread_phase.store(phase == 1 ? 2 : -1, std::memory_order_release);
  return nullptr;
}

void* RecordThreadStackSize(void*) {
  pthread_attr_t attributes;
  if (pthread_getattr_np(pthread_self(), &attributes) != 0) {
    return nullptr;
  }
  size_t stack_size = 0;
  if (pthread_attr_getstacksize(&attributes, &stack_size) == 0) {
    g_observed_stack_size.store(stack_size, std::memory_order_release);
  }
  pthread_attr_destroy(&attributes);
  return nullptr;
}

TEST(BionicPthreadCreateRuntimeTest, InitializesGuestThreadBeforeEntryPoint) {
  g_thread_phase.store(0, std::memory_order_release);
  ConfigureBionicPthreadThreadInitializer(InitializeThread);

  pthread_t thread{};
  ASSERT_EQ(mocktail_bionic_pthread_create(&thread, nullptr,
                                           CheckInitializedThread, nullptr),
            0);
  ASSERT_EQ(pthread_join(thread, nullptr), 0);
  EXPECT_EQ(g_thread_phase.load(std::memory_order_acquire), 2);

  ConfigureBionicPthreadThreadInitializer(nullptr);
}

TEST(BionicPthreadCreateRuntimeTest, RejectsInvalidArguments) {
  pthread_t thread{};
  EXPECT_EQ(mocktail_bionic_pthread_create(nullptr, nullptr,
                                           CheckInitializedThread, nullptr),
            EINVAL);
  EXPECT_EQ(mocktail_bionic_pthread_create(&thread, nullptr, nullptr, nullptr),
            EINVAL);
}

TEST(BionicPthreadCreateRuntimeTest, UsesExactBionicStackForNullAttributes) {
  g_observed_stack_size.store(0, std::memory_order_release);

  pthread_t thread{};
  ASSERT_EQ(mocktail_bionic_pthread_create(&thread, nullptr,
                                           RecordThreadStackSize, nullptr),
            0);
  ASSERT_EQ(pthread_join(thread, nullptr), 0);
  ExpectRequestedStackSize(
      g_observed_stack_size.load(std::memory_order_acquire),
      kBionicLp64DefaultThreadStackSize);
}

TEST(BionicPthreadCreateRuntimeTest, RetriesRejectedNullAttributeStack) {
  pthread_attr_t host_defaults;
  ASSERT_EQ(pthread_attr_init(&host_defaults), 0);
  size_t host_default_stack_size = 0;
  ASSERT_EQ(pthread_attr_getstacksize(&host_defaults, &host_default_stack_size),
            0);
  ASSERT_EQ(pthread_attr_destroy(&host_defaults), 0);

  g_intercepted_create_calls.store(0, std::memory_order_release);
  g_forced_create_failures.store(1, std::memory_order_release);
  g_first_create_stack_size.store(0, std::memory_order_release);
  g_second_create_stack_size.store(0, std::memory_order_release);
  g_third_create_stack_size.store(0, std::memory_order_release);
  g_intercept_pthread_create.store(true, std::memory_order_release);

  pthread_t thread{};
  const int result = mocktail_bionic_pthread_create(
      &thread, nullptr, RecordThreadStackSize, nullptr);
  g_intercept_pthread_create.store(false, std::memory_order_release);

  ASSERT_EQ(result, 0);
  ASSERT_EQ(pthread_join(thread, nullptr), 0);
  EXPECT_EQ(g_intercepted_create_calls.load(std::memory_order_acquire), 2);
  EXPECT_EQ(g_first_create_stack_size.load(std::memory_order_acquire),
            kBionicLp64DefaultThreadStackSize);
  EXPECT_EQ(
      g_second_create_stack_size.load(std::memory_order_acquire),
      std::max(host_default_stack_size, kBionicLp64FallbackThreadStackSize));
}

TEST(BionicPthreadCreateRuntimeTest, GrowsRejectedExplicitGuestStack) {
  constexpr size_t kRequestedStackSize = 512U * 1024U;
  pthread_attr_t host_defaults;
  ASSERT_EQ(pthread_attr_init(&host_defaults), 0);
  size_t host_default_stack_size = 0;
  ASSERT_EQ(pthread_attr_getstacksize(&host_defaults, &host_default_stack_size),
            0);
  ASSERT_EQ(pthread_attr_destroy(&host_defaults), 0);

  pthread_attr_t guest_attributes;
  ASSERT_EQ(pthread_attr_init(&guest_attributes), 0);
  ASSERT_EQ(pthread_attr_setstacksize(&guest_attributes, kRequestedStackSize),
            0);

  g_intercepted_create_calls.store(0, std::memory_order_release);
  g_forced_create_failures.store(2, std::memory_order_release);
  g_first_create_stack_size.store(0, std::memory_order_release);
  g_second_create_stack_size.store(0, std::memory_order_release);
  g_third_create_stack_size.store(0, std::memory_order_release);
  g_intercept_pthread_create.store(true, std::memory_order_release);

  pthread_t thread{};
  const int result = mocktail_bionic_pthread_create(
      &thread, &guest_attributes, RecordThreadStackSize, nullptr);
  g_intercept_pthread_create.store(false, std::memory_order_release);

  ASSERT_EQ(pthread_attr_destroy(&guest_attributes), 0);
  ASSERT_EQ(result, 0);
  ASSERT_EQ(pthread_join(thread, nullptr), 0);
  EXPECT_EQ(g_intercepted_create_calls.load(std::memory_order_acquire), 3);
  EXPECT_EQ(g_first_create_stack_size.load(std::memory_order_acquire),
            kRequestedStackSize);
  EXPECT_EQ(g_second_create_stack_size.load(std::memory_order_acquire),
            kRequestedStackSize);
  EXPECT_EQ(g_third_create_stack_size.load(std::memory_order_acquire),
            std::max({kRequestedStackSize, host_default_stack_size,
                      kBionicLp64FallbackThreadStackSize}));
}

TEST(BionicPthreadCreateRuntimeTest, PreservesRequestedStackSize) {
  constexpr size_t kRequestedStackSize = 2 * 1024 * 1024;
  g_observed_stack_size.store(0, std::memory_order_release);

  pthread_attr_t attributes;
  ASSERT_EQ(pthread_attr_init(&attributes), 0);
  ASSERT_EQ(pthread_attr_setstacksize(&attributes, kRequestedStackSize), 0);

  pthread_t thread{};
  ASSERT_EQ(mocktail_bionic_pthread_create(&thread, &attributes,
                                           RecordThreadStackSize, nullptr),
            0);
  EXPECT_EQ(pthread_attr_destroy(&attributes), 0);
  ASSERT_EQ(pthread_join(thread, nullptr), 0);
  ExpectRequestedStackSize(
      g_observed_stack_size.load(std::memory_order_acquire),
      kRequestedStackSize);
}

TEST(BionicPthreadCreateRuntimeTest, RetryPreservesRequestedStackSize) {
  constexpr size_t kRequestedStackSize = 3 * 1024 * 1024;
  g_observed_stack_size.store(0, std::memory_order_release);

  pthread_attr_t attributes;
  ASSERT_EQ(pthread_attr_init(&attributes), 0);
  ASSERT_EQ(pthread_attr_setstacksize(&attributes, kRequestedStackSize), 0);
  ASSERT_EQ(pthread_attr_setinheritsched(&attributes, PTHREAD_EXPLICIT_SCHED),
            0);
  ASSERT_EQ(pthread_attr_setschedpolicy(&attributes, SCHED_FIFO), 0);
  // A zero FIFO priority makes the fully copied host attr invalid. The
  // compatibility retry drops advisory scheduling while retaining the stack.
  sched_param invalid_parameters{};
  ASSERT_EQ(pthread_attr_getschedparam(&attributes, &invalid_parameters), 0);
  ASSERT_EQ(invalid_parameters.sched_priority, 0);

  pthread_t thread{};
  ASSERT_EQ(mocktail_bionic_pthread_create(&thread, &attributes,
                                           RecordThreadStackSize, nullptr),
            0);
  EXPECT_EQ(pthread_attr_destroy(&attributes), 0);
  ASSERT_EQ(pthread_join(thread, nullptr), 0);
  ExpectRequestedStackSize(
      g_observed_stack_size.load(std::memory_order_acquire),
      kRequestedStackSize);
}

}  // namespace
}  // namespace mocktail::compat
