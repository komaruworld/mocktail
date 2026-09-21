#include "compat/bionic_pthread_create_runtime.h"

#include "compat/bionic_abi_exports.h"

#include <errno.h>
#include <stdio.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <limits>
#include <new>

#if defined(__GLIBC__)
#include <dlfcn.h>
#endif

namespace mocktail::compat {
namespace {

struct ThreadStartContext {
  void* (*start_routine)(void*) = nullptr;
  void* argument = nullptr;
  NativeThreadInitializer initializer = nullptr;
};

std::atomic<NativeThreadInitializer> g_thread_initializer{nullptr};

// Roblox requests maximum-priority FIFO for its Main thread. On hosts with
// RLIMIT_RTPRIO=99 this succeeds and later trips the finite RLIMIT_RTTIME.
bool RejectRealtimeScheduling(int policy) noexcept {
  const int base_policy = policy & ~SCHED_RESET_ON_FORK;
  if (base_policy != SCHED_FIFO && base_policy != SCHED_RR) {
    return false;
  }
  static std::atomic_flag logged = ATOMIC_FLAG_INIT;
  if (!logged.test_and_set(std::memory_order_relaxed)) {
    const int saved_errno = errno;
    fprintf(stderr, "  [scheduler] denied guest real-time scheduling (policy=%d): "
                    "keeping host scheduling to prevent RLIMIT_RTTIME SIGXCPU\n",
            policy);
    errno = saved_errno;
  }
  return true;
}

size_t HostStackSizeForGuest(size_t guest_stack_size) noexcept {
#if defined(__GLIBC__)
  using GetStaticTlsInfo = void (*)(size_t*, size_t*);
  static const auto get_static_tls_info =
      reinterpret_cast<GetStaticTlsInfo>(
          dlsym(RTLD_DEFAULT, "_dl_get_tls_static_info"));
  size_t tls_size = 0;
  size_t tls_alignment = 0;
  if (get_static_tls_info != nullptr) {
    get_static_tls_info(&tls_size, &tls_alignment);
  }

  // glibc places static TLS inside the requested mapping. Compensate only
  // unusually large blocks; Mocktail's compatibility TLS is several MiB.
  constexpr size_t kLargeStaticTlsThreshold = 1U * 1024U * 1024U;
  if (tls_size > kLargeStaticTlsThreshold && tls_alignment != 0 &&
      (tls_alignment & (tls_alignment - 1)) == 0 &&
      tls_size <= std::numeric_limits<size_t>::max() - (tls_alignment - 1)) {
    const size_t reserve =
        (tls_size + tls_alignment - 1) & ~(tls_alignment - 1);
    if (guest_stack_size <= std::numeric_limits<size_t>::max() - reserve) {
      return guest_stack_size + reserve;
    }
  }
#endif
  return guest_stack_size;
}

// Guest pthread_attr_t buffers use the Bionic LP64 layout; they are never a
// host pthread_attr_t, so decode the fixed fields directly.
int GuestDetachState(const MocktailBionicPthreadAttr& source) noexcept {
  return (source.flags & kMocktailBionicAttrFlagDetached) != 0
             ? PTHREAD_CREATE_DETACHED
             : PTHREAD_CREATE_JOINABLE;
}

size_t GuestStackSize(const MocktailBionicPthreadAttr& source) noexcept {
  return source.stack_size != 0 ? source.stack_size
                                : kBionicLp64DefaultThreadStackSize;
}

int CopySupportedThreadAttributes(const MocktailBionicPthreadAttr& source,
                                  pthread_attr_t* destination) noexcept {
  int result =
      pthread_attr_setdetachstate(destination, GuestDetachState(source));
  if (result != 0) {
    return result;
  }

  result = pthread_attr_setguardsize(destination, source.guard_size);
  if (result != 0) {
    return result;
  }

  // The compatibility surface exposes pthread_attr_setstacksize, but not
  // pthread_attr_setstack. Copying pthread_attr_getstack's address is unsafe:
  // glibc represents an automatically allocated stack with a synthetic
  // address derived from its size.
  result = pthread_attr_setstacksize(
      destination, HostStackSizeForGuest(GuestStackSize(source)));
  if (result != 0) {
    return result;
  }

  // Bionic applies the attribute's scheduling policy at creation whenever it
  // differs from the normal policy.
  if (RejectRealtimeScheduling(source.sched_policy)) {
    return EPERM;
  }
  if (source.sched_policy != SCHED_NORMAL) {
    result = pthread_attr_setschedpolicy(destination, source.sched_policy);
    if (result != 0) {
      return result;
    }
    sched_param scheduler_parameters{};
    scheduler_parameters.sched_priority = source.sched_priority;
    result = pthread_attr_setschedparam(destination, &scheduler_parameters);
    if (result != 0) {
      return result;
    }
    return pthread_attr_setinheritsched(destination, PTHREAD_EXPLICIT_SCHED);
  }

  return pthread_attr_setinheritsched(destination, PTHREAD_INHERIT_SCHED);
}

int CopyRequiredThreadAttributes(const MocktailBionicPthreadAttr& source,
                                 pthread_attr_t* destination) noexcept {
  int result =
      pthread_attr_setdetachstate(destination, GuestDetachState(source));
  if (result != 0) {
    return result;
  }

  // The retry must retain an explicitly requested guest stack. Falling back
  // to a null/default host attr is unsafe on musl, whose default stack is much
  // smaller than Bionic's and can be exhausted by a single libroblox frame.
  return pthread_attr_setstacksize(
      destination, HostStackSizeForGuest(GuestStackSize(source)));
}

int ConfigureHostSafeStackFallback(const MocktailBionicPthreadAttr* source,
                                   pthread_attr_t* destination) noexcept {
  size_t guest_stack_size = 0;
  if (source != nullptr) {
    int result =
        pthread_attr_setdetachstate(destination, GuestDetachState(*source));
    if (result != 0) {
      return result;
    }
    guest_stack_size = GuestStackSize(*source);
  }

  size_t host_default_stack_size = 0;
  int result = pthread_attr_getstacksize(destination, &host_default_stack_size);
  if (result != 0) {
    return result;
  }
  const size_t guest_floor =
      std::max(guest_stack_size, kBionicLp64FallbackThreadStackSize);
  return pthread_attr_setstacksize(
      destination,
      std::max(host_default_stack_size, HostStackSizeForGuest(guest_floor)));
}

void* RunGuestThread(void* raw_context) noexcept {
  auto* context = static_cast<ThreadStartContext*>(raw_context);
  const ThreadStartContext values = *context;
  delete context;

  if (values.initializer != nullptr) {
    values.initializer();
  }
  return values.start_routine(values.argument);
}

}  // namespace

void ConfigureBionicPthreadThreadInitializer(
    NativeThreadInitializer initializer) noexcept {
  g_thread_initializer.store(initializer, std::memory_order_release);
}

int CreateBionicPthread(pthread_t* thread,
                        const MocktailBionicPthreadAttr* attr,
                        void* (*start_routine)(void*), void* argument) {
  if (thread == nullptr || start_routine == nullptr) {
    return EINVAL;
  }

  auto* context = new (std::nothrow) ThreadStartContext;
  if (context == nullptr) {
    return EAGAIN;
  }
  context->start_routine = start_routine;
  context->argument = argument;
  context->initializer = g_thread_initializer.load(std::memory_order_acquire);

  pthread_attr_t normalized_attr;
  const pthread_attr_t* host_attr = &normalized_attr;
  bool normalized_attr_initialized = false;
  int result = pthread_attr_init(&normalized_attr);
  if (result == 0) {
    normalized_attr_initialized = true;
    if (attr == nullptr) {
      result = pthread_attr_setstacksize(
          &normalized_attr,
          HostStackSizeForGuest(kBionicLp64DefaultThreadStackSize));
    } else {
      result = CopySupportedThreadAttributes(*attr, &normalized_attr);
    }
  }

  if (result == 0) {
    result = pthread_create(thread, host_attr, RunGuestThread, context);
  }
  if (normalized_attr_initialized) {
    pthread_attr_destroy(&normalized_attr);
  }

  // Guest attributes can encode libc-private state that the host rejects only
  // at pthread_create(). First retry explicit attributes with only required
  // portable semantics, preserving their requested stack exactly. A null
  // guest attr skips directly to the host-safe stack fallback.
  if (result == EINVAL) {
    pthread_attr_t portable_attr;
    result = pthread_attr_init(&portable_attr);
    if (result == 0) {
      result = attr == nullptr
                   ? ConfigureHostSafeStackFallback(nullptr, &portable_attr)
                   : CopyRequiredThreadAttributes(*attr, &portable_attr);
      if (result == 0) {
        result =
            pthread_create(thread, &portable_attr, RunGuestThread, context);
      }
      pthread_attr_destroy(&portable_attr);
    }
  }

  // Loading Android DSOs can enlarge the host's static-TLS reservation until
  // an otherwise valid Bionic stack falls below pthread_create's effective
  // minimum. If the exact portable retry is still rejected, grow only the
  // stack while retaining the requested detach state. The larger of the guest
  // request, host default, and musl-safe floor is compatible with POSIX's
  // minimum-stack contract. Failed creates never start the routine, so this
  // call continues to own the context through the final retry.
  if (result == EINVAL && attr != nullptr) {
    pthread_attr_t fallback_attr;
    result = pthread_attr_init(&fallback_attr);
    if (result == 0) {
      result = ConfigureHostSafeStackFallback(attr, &fallback_attr);
      if (result == 0) {
        result =
            pthread_create(thread, &fallback_attr, RunGuestThread, context);
      }
      pthread_attr_destroy(&fallback_attr);
    }
  }
  if (result != 0) {
    delete context;
  }
  return result;
}

}  // namespace mocktail::compat

extern "C" {

int mocktail_pthread_attr_init(MocktailBionicPthreadAttr* attr) {
  if (attr == nullptr) {
    return EINVAL;
  }
  attr->flags = 0;
  attr->stack_base = nullptr;
  attr->stack_size =
      mocktail::compat::kBionicLp64DefaultThreadStackSize;
  const long page_size = sysconf(_SC_PAGESIZE);
  attr->guard_size = page_size > 0 ? static_cast<size_t>(page_size) : 0;
  attr->sched_policy = SCHED_OTHER;
  attr->sched_priority = 0;
  std::memset(attr->reserved, 0, sizeof(attr->reserved));
  return 0;
}

int mocktail_pthread_attr_destroy(MocktailBionicPthreadAttr* attr) {
  (void)attr;
  return 0;
}

int mocktail_pthread_attr_setstacksize(MocktailBionicPthreadAttr* attr,
                                       size_t stack_size) {
  if (attr == nullptr || stack_size < PTHREAD_STACK_MIN) {
    return EINVAL;
  }
  attr->stack_size = stack_size;
  attr->flags &= ~kMocktailBionicAttrFlagUserStack;
  return 0;
}

int mocktail_pthread_attr_setdetachstate(MocktailBionicPthreadAttr* attr,
                                         int detach_state) {
  if (attr == nullptr || (detach_state != PTHREAD_CREATE_DETACHED &&
                          detach_state != PTHREAD_CREATE_JOINABLE)) {
    return EINVAL;
  }
  if (detach_state == PTHREAD_CREATE_DETACHED) {
    attr->flags |= kMocktailBionicAttrFlagDetached;
  } else {
    attr->flags &= ~kMocktailBionicAttrFlagDetached;
  }
  return 0;
}

int mocktail_pthread_attr_setschedparam(MocktailBionicPthreadAttr* attr,
                                        const struct sched_param* parameters) {
  if (attr == nullptr || parameters == nullptr) {
    return EINVAL;
  }
  attr->sched_priority = parameters->sched_priority;
  return 0;
}

int mocktail_pthread_getattr_np(MocktailBionicPthreadAttr* attr) {
  if (attr == nullptr) {
    return EINVAL;
  }
  pthread_attr_t host_attr;
  int result = pthread_getattr_np(pthread_self(), &host_attr);
  if (result != 0) {
    return result;
  }
  void* stack_base = nullptr;
  size_t stack_size = 0;
  result = pthread_attr_getstack(&host_attr, &stack_base, &stack_size);
  size_t guard_size = 0;
  if (result == 0) {
    result = pthread_attr_getguardsize(&host_attr, &guard_size);
  }
  int detach_state = PTHREAD_CREATE_JOINABLE;
  if (result == 0) {
    result = pthread_attr_getdetachstate(&host_attr, &detach_state);
  }
  sched_param parameters{};
  int policy = SCHED_OTHER;
  if (result == 0) {
    result = pthread_getschedparam(pthread_self(), &policy, &parameters);
  }
  pthread_attr_destroy(&host_attr);
  if (result != 0) {
    return result;
  }
  attr->flags = detach_state == PTHREAD_CREATE_DETACHED
                    ? kMocktailBionicAttrFlagDetached
                    : 0;
  attr->stack_base = stack_base;
  attr->stack_size = stack_size;
  attr->guard_size = guard_size;
  attr->sched_policy = policy;
  attr->sched_priority = parameters.sched_priority;
  std::memset(attr->reserved, 0, sizeof(attr->reserved));
  return 0;
}

int mocktail_pthread_attr_getstack(const MocktailBionicPthreadAttr* attr,
                                   void** stack_base, size_t* stack_size) {
  if (attr == nullptr) {
    return EINVAL;
  }
  if (stack_base != nullptr) {
    *stack_base = attr->stack_base;
  }
  if (stack_size != nullptr) {
    *stack_size = attr->stack_size;
  }
  return 0;
}


}  // extern "C"

extern "C" int mocktail_bionic_pthread_create(
    pthread_t* thread, const MocktailBionicPthreadAttr* attr,
    void* (*start_routine)(void*), void* argument) {
  return mocktail::compat::CreateBionicPthread(thread, attr, start_routine,
                                               argument);
}

extern "C" int mocktail_bionic_pthread_setschedparam(
    pthread_t thread, int policy, const struct sched_param* parameters) {
  if (mocktail::compat::RejectRealtimeScheduling(policy)) {
    return EPERM;
  }
  return pthread_setschedparam(thread, policy, parameters);
}

extern "C" int mocktail_bionic_sched_setscheduler(
    pid_t tid, int policy, const struct sched_param* parameters) {
  if (mocktail::compat::RejectRealtimeScheduling(policy)) {
    errno = EPERM;
    return -1;
  }
  return sched_setscheduler(tid, policy, parameters);
}
