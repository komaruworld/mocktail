#include "compat/bionic_pthread_create_runtime.h"

#include <errno.h>

#include <algorithm>
#include <atomic>
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

int CopySupportedThreadAttributes(const pthread_attr_t& source,
                                  pthread_attr_t* destination) noexcept {
  int detach_state = PTHREAD_CREATE_JOINABLE;
  int result = pthread_attr_getdetachstate(&source, &detach_state);
  if (result != 0) {
    return result;
  }
  result = pthread_attr_setdetachstate(destination, detach_state);
  if (result != 0) {
    return result;
  }

  size_t guard_size = 0;
  result = pthread_attr_getguardsize(&source, &guard_size);
  if (result != 0) {
    return result;
  }
  result = pthread_attr_setguardsize(destination, guard_size);
  if (result != 0) {
    return result;
  }

  // The compatibility surface exposes pthread_attr_setstacksize, but not
  // pthread_attr_setstack. Copying pthread_attr_getstack's address is unsafe:
  // glibc represents an automatically allocated stack with a synthetic
  // address derived from its size.
  size_t stack_size = 0;
  result = pthread_attr_getstacksize(&source, &stack_size);
  if (result != 0) {
    return result;
  }
  result = pthread_attr_setstacksize(destination,
                                     HostStackSizeForGuest(stack_size));
  if (result != 0) {
    return result;
  }

  int inherit_scheduler = PTHREAD_INHERIT_SCHED;
  result = pthread_attr_getinheritsched(&source, &inherit_scheduler);
  if (result != 0) {
    return result;
  }
  if (inherit_scheduler == PTHREAD_EXPLICIT_SCHED) {
    int scheduler_policy = 0;
    sched_param scheduler_parameters{};
    result = pthread_attr_getschedpolicy(&source, &scheduler_policy);
    if (result == 0) {
      result = pthread_attr_getschedparam(&source, &scheduler_parameters);
    }
    if (result == 0) {
      result = pthread_attr_setschedpolicy(destination, scheduler_policy);
    }
    if (result == 0) {
      result = pthread_attr_setschedparam(destination, &scheduler_parameters);
    }
    if (result != 0) {
      return result;
    }
  }
  return pthread_attr_setinheritsched(destination, inherit_scheduler);
}

int CopyRequiredThreadAttributes(const pthread_attr_t& source,
                                 pthread_attr_t* destination) noexcept {
  int detach_state = PTHREAD_CREATE_JOINABLE;
  int result = pthread_attr_getdetachstate(&source, &detach_state);
  if (result != 0) {
    return result;
  }
  result = pthread_attr_setdetachstate(destination, detach_state);
  if (result != 0) {
    return result;
  }

  // The retry must retain an explicitly requested guest stack. Falling back
  // to a null/default host attr is unsafe on musl, whose default stack is much
  // smaller than Bionic's and can be exhausted by a single libroblox frame.
  size_t stack_size = 0;
  result = pthread_attr_getstacksize(&source, &stack_size);
  if (result != 0) {
    return result;
  }
  return pthread_attr_setstacksize(destination,
                                   HostStackSizeForGuest(stack_size));
}

int ConfigureHostSafeStackFallback(const pthread_attr_t* source,
                                   pthread_attr_t* destination) noexcept {
  size_t guest_stack_size = 0;
  if (source != nullptr) {
    int detach_state = PTHREAD_CREATE_JOINABLE;
    int result = pthread_attr_getdetachstate(source, &detach_state);
    if (result != 0) {
      return result;
    }
    result = pthread_attr_setdetachstate(destination, detach_state);
    if (result != 0) {
      return result;
    }
    result = pthread_attr_getstacksize(source, &guest_stack_size);
    if (result != 0) {
      return result;
    }
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

int CreateBionicPthread(pthread_t* thread, const pthread_attr_t* attr,
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

extern "C" int mocktail_bionic_pthread_create(pthread_t* thread,
                                               const pthread_attr_t* attr,
                                               void* (*start_routine)(void*),
                                               void* argument) {
  return mocktail::compat::CreateBionicPthread(thread, attr, start_routine,
                                               argument);
}
