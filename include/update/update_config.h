#ifndef MOCKTAIL_UPDATE_UPDATE_CONFIG_H_
#define MOCKTAIL_UPDATE_UPDATE_CONFIG_H_

#include <filesystem>
#include <string>
#include <vector>

namespace mocktail::update {

struct UpdateConfig {
  bool automatic = true;
  bool launch_after_update = false;
  bool mocktail_release_check = true;
  std::string source = "apk-pure";
};

struct UpdateConfigResult {
  UpdateConfig config;
  bool file_loaded = false;
  std::vector<std::string> warnings;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

// Other top-level sections are handled by runtime_config_file.cc.
UpdateConfigResult LoadUpdateConfig(const std::filesystem::path& path);

}  // namespace mocktail::update

#endif  // MOCKTAIL_UPDATE_UPDATE_CONFIG_H_
