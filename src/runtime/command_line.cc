#include "runtime/command_line.h"

#include <cstdlib>
#include <sstream>
#include <string>
#include <string_view>

#include "runtime/roblox_launch_uri.h"

namespace mocktail {
namespace runtime {
namespace {

bool ReadOptionValue(int argc, const char* const argv[], int* index,
                     const std::string& option, std::string* value,
                     std::string* error) {
  if (*index + 1 >= argc || argv[*index + 1] == nullptr ||
      argv[*index + 1][0] == '\0' ||
      std::string(argv[*index + 1]).rfind("--", 0) == 0) {
    *error = "missing value for " + option;
    return false;
  }
  *value = argv[++(*index)];
  return true;
}

bool SetEnvironment(const char* name, const std::string& value,
                    std::string* error) {
  if (setenv(name, value.c_str(), 1) == 0) {
    return true;
  }
  if (error != nullptr) {
    *error = std::string("could not apply command-line option: ") + name;
  }
  return false;
}

void SecureErase(char* data, std::size_t size) {
  volatile char* bytes = data;
  for (std::size_t index = 0; bytes != nullptr && index < size; ++index) {
    bytes[index] = '\0';
  }
}

}  // namespace

CommandLineParseResult ParseCommandLine(int argc, const char* const argv[]) {
  CommandLineParseResult result;
  if (argc > 0 && argv != nullptr && argv[0] != nullptr && argv[0][0] != '\0') {
    result.options.program_name = argv[0];
  }
  for (int index = 1; index < argc; ++index) {
    if (argv == nullptr || argv[index] == nullptr) {
      result.error = "invalid null command-line argument";
      return result;
    }
    const std::string argument = argv[index];
    if (argument == "--help" || argument == "-h") {
      result.options.mode = CommandMode::kHelp;
      return result;
    }
    if (argument == "--roblox-lib") {
      if (!ReadOptionValue(argc, argv, &index, argument,
                           &result.options.roblox_library_path,
                           &result.error)) {
        return result;
      }
    } else if (argument == "--headless") {
      result.options.window_mode = WindowMode::kHeadless;
    } else if (argument == "--windowed") {
      result.options.window_mode = WindowMode::kWindowed;
    } else if (argument == "--graphics") {
      if (!ReadOptionValue(argc, argv, &index, argument,
                           &result.options.graphics_backend, &result.error)) {
        return result;
      }
    } else if (argument == "--allow-unverified-build") {
      result.options.allow_unverified_build = true;
    } else if (argument == "--launch-uri") {
      if (!result.options.launch_request_json.empty()) {
        result.error = "duplicate option: --launch-uri";
        return result;
      }
      if (!ReadOptionValue(argc, argv, &index, argument,
                           &result.options.raw_launch_argument,
                           &result.error)) {
        return result;
      }
      RobloxExperienceLaunchRequest launch_request;
      const Status launch_status = ParseRobloxLaunchUri(
          result.options.raw_launch_argument, &launch_request);
      if (!launch_status.ok()) {
        result.options.raw_launch_argument.clear();
        result.error = "invalid Roblox launch URI: " + launch_status.message();
        return result;
      }
      result.options.launch_request_json = launch_request.canonical_json;
      result.options.launch_argument_index = index;
    } else if (argument == "--launch-request-json") {
      if (!result.options.launch_request_json.empty()) {
        result.error = "duplicate controlled launch request";
        return result;
      }
      std::string document;
      if (!ReadOptionValue(argc, argv, &index, argument, &document,
                           &result.error)) {
        return result;
      }
      RobloxExperienceLaunchRequest launch_request;
      const Status launch_status =
          ParseRobloxExperienceLaunchJson(document, &launch_request);
      if (!launch_status.ok()) {
        result.error = "invalid controlled launch request";
        return result;
      }
      result.options.raw_launch_argument = std::move(document);
      result.options.launch_request_json = launch_request.canonical_json;
      result.options.launch_argument_index = index;
    } else if (!argument.empty() && argument.front() != '-' &&
               result.options.launch_request_json.empty()) {
      RobloxExperienceLaunchRequest launch_request;
      const Status launch_status =
          ParseRobloxLaunchUri(argument, &launch_request);
      if (!launch_status.ok()) {
        result.error = "unknown argument";
        return result;
      }
      result.options.raw_launch_argument = argument;
      result.options.launch_request_json = launch_request.canonical_json;
      result.options.launch_argument_index = index;
    } else {
      result.error = !argument.empty() && argument.front() != '-'
                         ? "unknown argument"
                         : "unknown option: " + argument;
      return result;
    }
  }
  return result;
}

bool BuildCommandLineReexecArguments(const CommandLineOptions& options,
                                     int argc, const char* const argv[],
                                     std::vector<std::string>* arguments,
                                     std::string* error) {
  if (arguments == nullptr || argc < 0 || (argc > 0 && argv == nullptr)) {
    if (error != nullptr) *error = "cannot prepare cgroup re-exec arguments";
    return false;
  }
  arguments->clear();
  arguments->reserve(static_cast<std::size_t>(argc) + 1);
  const bool replace_raw_launch = !options.raw_launch_argument.empty();
  if (replace_raw_launch &&
      (options.launch_request_json.empty() ||
       options.launch_argument_index <= 0 ||
       options.launch_argument_index >= argc ||
       argv[options.launch_argument_index] == nullptr ||
       options.raw_launch_argument != argv[options.launch_argument_index])) {
    if (error != nullptr) *error = "cannot normalize cgroup launch request";
    return false;
  }
  bool replaced = false;
  for (int index = 1; index < argc; ++index) {
    if (argv[index] == nullptr) {
      arguments->clear();
      if (error != nullptr) *error = "cannot prepare cgroup re-exec arguments";
      return false;
    }
    if (replace_raw_launch && index + 1 == options.launch_argument_index &&
        (std::string_view(argv[index]) == "--launch-uri" ||
         std::string_view(argv[index]) == "--launch-request-json")) {
      arguments->emplace_back("--launch-request-json");
      arguments->push_back(options.launch_request_json);
      ++index;
      replaced = true;
    } else if (replace_raw_launch && index == options.launch_argument_index) {
      arguments->emplace_back("--launch-request-json");
      arguments->push_back(options.launch_request_json);
      replaced = true;
    } else {
      arguments->emplace_back(argv[index]);
    }
  }
  if (replace_raw_launch && !replaced) {
    arguments->clear();
    if (error != nullptr) *error = "cannot normalize cgroup launch request";
    return false;
  }
  if (error != nullptr) error->clear();
  return true;
}

void ScrubCommandLineLaunchArguments(CommandLineOptions* options, int argc,
                                     char* argv[]) {
  if (options == nullptr || options->raw_launch_argument.empty()) return;
  const int argument_index = options->launch_argument_index;
  if (argv != nullptr && argument_index > 0 && argument_index < argc &&
      argv[argument_index] != nullptr) {
    SecureErase(argv[argument_index], options->raw_launch_argument.size());
  }
  SecureErase(options->raw_launch_argument.data(),
              options->raw_launch_argument.size());
  options->raw_launch_argument.clear();
  SecureErase(options->launch_request_json.data(),
              options->launch_request_json.size());
  options->launch_request_json.clear();
  options->launch_argument_index = -1;
}

bool ApplyCommandLineEnvironment(const CommandLineOptions& options,
                                 std::string* error) {
  if (!options.roblox_library_path.empty() &&
      !SetEnvironment("ROBLOX_LIB_PATH", options.roblox_library_path, error)) {
    return false;
  }
  if (!options.graphics_backend.empty() &&
      !SetEnvironment("MOCKTAIL_GRAPHICS_BACKEND", options.graphics_backend,
                      error)) {
    return false;
  }
  if (options.window_mode == WindowMode::kHeadless) {
    return SetEnvironment("MOCKTAIL_HEADLESS", "1", error);
  }
  if (options.window_mode == WindowMode::kWindowed) {
    return SetEnvironment("MOCKTAIL_HEADLESS", "0", error);
  }
  return true;
}

std::string CommandLineUsage(const std::string& program_name) {
  std::ostringstream usage;
  usage
      << "Usage: " << (program_name.empty() ? "mocktail" : program_name)
      << " [options] [roblox-uri]\n\n"
      << "Normal startup downloads and activates a verified managed x86_64 "
         "Roblox payload automatically.\n\n"
      << "Options:\n"
      << "  --roblox-lib <path>      Development override for a specific "
         "x86_64 libroblox.so\n"
      << "  --headless               Run without creating an SDL window\n"
      << "  --windowed               Force windowed startup (default)\n"
      << "  --graphics <backend>     direct-vulkan | opengl | system | "
         "angle-vulkan (default: direct-vulkan)\n"
      << "  --allow-unverified-build Run a known but unverified Build-ID "
         "profile\n"
      << "  --launch-uri <uri>       Join from a roblox: or roblox-player: "
         "website link\n"
      << "  --help, -h               Show this help\n\n"
      << "Auth:\n"
      << "  When no saved Roblox cookie is found, Roblox starts in guest mode "
         "and\n"
      << "  uses its native sign-in flow. New credentials are stored "
         "privately.\n\n"
      << "Additional runtime options are available as MOCKTAIL_* environment "
         "variables.\n";
  return usage.str();
}

}  // namespace runtime
}  // namespace mocktail
