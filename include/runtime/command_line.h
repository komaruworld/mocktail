#ifndef MOCKTAIL_RUNTIME_COMMAND_LINE_H_
#define MOCKTAIL_RUNTIME_COMMAND_LINE_H_

#include <string>
#include <vector>

namespace mocktail {
namespace runtime {

enum class CommandMode {
  kRun,
  kHelp,
};

enum class WindowMode {
  kUnspecified,
  kHeadless,
  kWindowed,
};

struct CommandLineOptions {
  CommandMode mode = CommandMode::kRun;
  WindowMode window_mode = WindowMode::kUnspecified;
  std::string program_name = "mocktail";
  std::string roblox_library_path;
  std::string graphics_backend;
  // Raw browser/internal input exists only until the composition root creates
  // safe re-exec arguments, then it is overwritten in this string and argv.
  std::string raw_launch_argument;
  std::string launch_request_json;
  int launch_argument_index = -1;
  bool allow_unverified_build = false;
};

struct CommandLineParseResult {
  CommandLineOptions options;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

CommandLineParseResult ParseCommandLine(int argc, const char* const argv[]);
// Produces argv[1..] for a possible cgroup re-exec. A raw website URI is
// replaced with the already-normalized internal request, so browser gameinfo
// tickets do not cross the exec boundary.
bool BuildCommandLineReexecArguments(const CommandLineOptions& options,
                                     int argc, const char* const argv[],
                                     std::vector<std::string>* arguments,
                                     std::string* error);
// Overwrites the original raw launch argument and both owned launch strings
// after the request and sanitized re-exec argv have been created.
void ScrubCommandLineLaunchArguments(CommandLineOptions* options, int argc,
                                     char* argv[]);
bool ApplyCommandLineEnvironment(const CommandLineOptions& options,
                                 std::string* error);
std::string CommandLineUsage(const std::string& program_name);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_COMMAND_LINE_H_
