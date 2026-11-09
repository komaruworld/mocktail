#include "runtime/process_diagnostics.h"

#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include <cerrno>
#include <cstddef>
#include <cstdint>

namespace mocktail::runtime {
namespace {

volatile sig_atomic_t current_stage = 0;

const char* StageName() {
  switch (current_stage) {
    case 1: return "native-runtime";
    case 2: return "shutdown";
    default: return "startup";
  }
}

// All helpers used by the signal handler use fixed storage and system calls;
// no iostreams, allocation, unwinding, or locks in the crashing thread.
void WriteBytes(const char* bytes, std::size_t size) {
  while (size > 0) {
    const ssize_t count = write(STDERR_FILENO, bytes, size);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) return;
    bytes += count;
    size -= static_cast<std::size_t>(count);
  }
}

void Text(const char* text) {
  std::size_t length = 0;
  while (text[length] != '\0') ++length;
  WriteBytes(text, length);
}

void Number(std::uint64_t value, unsigned base = 10) {
  char buffer[32];
  std::size_t start = sizeof(buffer);
  do {
    buffer[--start] = "0123456789abcdef"[value % base];
    value /= base;
  } while (value != 0);
  WriteBytes(buffer + start, sizeof(buffer) - start);
}

void SignedNumber(long value) {
  if (value < 0) {
    Text("-");
    Number(static_cast<std::uint64_t>(-(value + 1)) + 1);
  } else {
    Number(static_cast<std::uint64_t>(value));
  }
}

void CpuTime(const char* label, clockid_t clock) {
  Text(label);
  timespec time{};
  if (clock_gettime(clock, &time) == 0) {
    Number(static_cast<std::uint64_t>(time.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(time.tv_nsec));
  } else {
    Text("unavailable");
  }
}

void DumpFile(const char* path, std::size_t maximum_bytes) {
  Text("[diagnostic] ");
  Text(path);
  Text("\n");
  const int descriptor = open(path, O_RDONLY | O_CLOEXEC);
  if (descriptor < 0) {
    Text("unavailable errno=");
    SignedNumber(errno);
    Text("\n");
    return;
  }
  char buffer[2048];
  while (maximum_bytes > 0) {
    const std::size_t capacity = maximum_bytes < sizeof(buffer)
                                     ? maximum_bytes : sizeof(buffer);
    const ssize_t count = read(descriptor, buffer, capacity);
    if (count < 0 && errno == EINTR) continue;
    if (count <= 0) break;
    WriteBytes(buffer, static_cast<std::size_t>(count));
    maximum_bytes -= static_cast<std::size_t>(count);
  }
  if (maximum_bytes == 0) Text("\n[diagnostic] file output capped\n");
  close(descriptor);
}

void Snapshot() {
  Text("[diagnostic] stage=");
  Text(StageName());
  Text(" pid=");
  Number(getpid());
  Text(" ppid=");
  Number(getppid());
  Text(" tid=");
  SignedNumber(syscall(SYS_gettid));
  Text(" scheduler=");
  SignedNumber(syscall(SYS_sched_getscheduler, 0));
  Text(" (OTHER=0 FIFO=1 RR=2 BATCH=3 IDLE=5; RESET_ON_FORK=1073741824)");
  CpuTime(" process_cpu_ns=", CLOCK_PROCESS_CPUTIME_ID);
  CpuTime(" thread_cpu_ns=", CLOCK_THREAD_CPUTIME_ID);
  CpuTime(" monotonic_ns=", CLOCK_MONOTONIC);
  Text("\n");
  DumpFile("/proc/self/limits", 8192);
  DumpFile("/proc/self/cgroup", 4096);
}

void CpuLimitSignal(int signal_number, siginfo_t* info, void* context) {
  Text("\n[crash] SIGXCPU signal=");
  SignedNumber(signal_number);
  Text(" si_code=");
  SignedNumber(info != nullptr ? info->si_code : 0);
  Text(" (SI_KERNEL=128 SI_USER=0 SI_TKILL=-6) ip=0x");
  std::uintptr_t instruction = 0;
  if (context != nullptr) {
#if defined(__x86_64__)
    instruction = static_cast<ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP];
#elif defined(__aarch64__)
    instruction = static_cast<ucontext_t*>(context)->uc_mcontext.pc;
#endif
  }
  Number(instruction, 16);
  Text("\n");
  Snapshot();
  DumpFile("/proc/thread-self/comm", 256);
  DumpFile("/proc/thread-self/sched", 8192);
  DumpFile("/proc/self/maps", 65536);
  Text("[crash] SIGXCPU diagnostics complete; preserving fatal signal. "
       "For SI_KERNEL, compare CPU time with Max cpu time and the failing "
       "thread's policy with Max realtime timeout. Collect coredumpctl info "
       "for this PID.\n");
  // SA_RESETHAND already restored SIG_DFL. The signal is blocked during this
  // handler and is delivered to this same thread when the handler returns.
  if (syscall(SYS_tgkill, getpid(), syscall(SYS_gettid), signal_number) < 0) {
    _exit(128 + signal_number);
  }
}

}  // namespace

void InstallCpuLimitDiagnostics() {
  struct sigaction previous{};
  if (sigaction(SIGXCPU, nullptr, &previous) != 0) {
    Text("[diagnostic] cannot inspect SIGXCPU handler\n");
    return;
  }
  if (previous.sa_handler != SIG_DFL) {
    Text("[diagnostic] existing SIGXCPU handler retained\n");
    return;
  }
  struct sigaction action{};
  action.sa_sigaction = CpuLimitSignal;
  action.sa_flags = SA_SIGINFO | SA_RESETHAND;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGXCPU, &action, nullptr) != 0) {
    Text("[diagnostic] cannot install SIGXCPU diagnostics\n");
    return;
  }
  Text("[diagnostic] SIGXCPU diagnostics enabled; output goes to console and "
       "the active session log\n");
}

void LogProcessDiagnostics(ProcessDiagnosticStage stage) {
  current_stage = static_cast<sig_atomic_t>(stage);
  Snapshot();
}

}  // namespace mocktail::runtime
