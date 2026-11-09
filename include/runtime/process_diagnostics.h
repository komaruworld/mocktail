#ifndef MOCKTAIL_RUNTIME_PROCESS_DIAGNOSTICS_H_
#define MOCKTAIL_RUNTIME_PROCESS_DIAGNOSTICS_H_

namespace mocktail::runtime {

enum class ProcessDiagnosticStage { kStartup, kNativeRuntime, kShutdown };

// Installs a diagnostic handler only if SIGXCPU still has its default action.
// The handler logs to stderr and re-delivers SIGXCPU with the default action.
void InstallCpuLimitDiagnostics();
void LogProcessDiagnostics(ProcessDiagnosticStage stage);

}  // namespace mocktail::runtime

#endif  // MOCKTAIL_RUNTIME_PROCESS_DIAGNOSTICS_H_
