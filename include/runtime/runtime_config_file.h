#ifndef MOCKTAIL_RUNTIME_RUNTIME_CONFIG_FILE_H_
#define MOCKTAIL_RUNTIME_RUNTIME_CONFIG_FILE_H_

#include <filesystem>
#include <string>

#include "runtime/environment.h"
#include "runtime/runtime_config.h"

namespace mocktail {
namespace runtime {

struct RuntimeConfigLoadResult {
  RuntimeConfig config;
  bool file_loaded = false;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

// Loads the optional YAML file as a defaults layer. Values already present in
// the process environment win over YAML; RuntimeConfig defaults are used last.
RuntimeConfigLoadResult LoadRuntimeConfig(
    const Environment& environment, const std::filesystem::path& config_file);

// Transitional composition helper. The legacy runtime still consumes the
// supported settings through environment variables, so the composition
// root exports one already-resolved RuntimeConfig before entering that
// boundary.
bool ExportRuntimeConfigEnvironment(const RuntimeConfig& config,
                                    std::string* error = nullptr);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_RUNTIME_CONFIG_FILE_H_
