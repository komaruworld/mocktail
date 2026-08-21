#include "compat/bionic_signal_runtime.h"

#include <cstring>

namespace mocktail::compat {
namespace {

void CopyGuestMaskToHost(std::uint64_t guest_mask, sigset_t* host_mask) {
  sigemptyset(host_mask);
  for (int signal_number = 1; signal_number <= 64; ++signal_number) {
    const std::uint64_t bit = std::uint64_t{1} << (signal_number - 1);
    if ((guest_mask & bit) != 0) {
      sigaddset(host_mask, signal_number);
    }
  }
}

std::uint64_t CopyHostMaskToGuest(const sigset_t& host_mask) {
  std::uint64_t guest_mask = 0;
  for (int signal_number = 1; signal_number <= 64; ++signal_number) {
    if (sigismember(&host_mask, signal_number) == 1) {
      guest_mask |= std::uint64_t{1} << (signal_number - 1);
    }
  }
  return guest_mask;
}

}  // namespace

int BionicSigactionCall(int signal_number, const BionicSigaction* action,
                        BionicSigaction* old_action) noexcept {
  struct sigaction host_action {};
  struct sigaction host_old_action {};
  struct sigaction* host_action_pointer = nullptr;
  if (action != nullptr) {
    host_action.sa_flags = action->flags;
    if ((action->flags & SA_SIGINFO) != 0) {
      host_action.sa_sigaction = action->callback.action;
    } else {
      host_action.sa_handler = action->callback.handler;
    }
    CopyGuestMaskToHost(action->mask, &host_action.sa_mask);
#if defined(__x86_64__)
    host_action.sa_restorer = action->restorer;
#endif
    host_action_pointer = &host_action;
  }

  const int result =
      sigaction(signal_number, host_action_pointer,
                old_action != nullptr ? &host_old_action : nullptr);
  if (result != 0 || old_action == nullptr) {
    return result;
  }

  *old_action = {};
  old_action->flags = host_old_action.sa_flags;
  if ((host_old_action.sa_flags & SA_SIGINFO) != 0) {
    old_action->callback.action = host_old_action.sa_sigaction;
  } else {
    old_action->callback.handler = host_old_action.sa_handler;
  }
  old_action->mask = CopyHostMaskToGuest(host_old_action.sa_mask);
#if defined(__x86_64__)
  old_action->restorer = host_old_action.sa_restorer;
#endif
  return 0;
}

}  // namespace mocktail::compat

extern "C" int mocktail_bionic_sigaction(
    int signal_number, const mocktail::compat::BionicSigaction* action,
    mocktail::compat::BionicSigaction* old_action) {
  return mocktail::compat::BionicSigactionCall(signal_number, action,
                                                old_action);
}
