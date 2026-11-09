#ifndef MOCKTAIL_RUNTIME_FLEASION_H_
#define MOCKTAIL_RUNTIME_FLEASION_H_

#include <filesystem>
#include <string>

#include "runtime/runtime_config.h"
#include "runtime/runtime_paths.h"

namespace mocktail::runtime {

struct FleasionPreparation {
  std::filesystem::path certificate;
  std::filesystem::path base_bundle;
  std::filesystem::path bundle;
  std::string error;
  explicit operator bool() const { return error.empty(); }
};

// Generates a private, replaceable trust bundle outside the managed payload.
// Reads certificates only; never reads Fleasion's private key or changes it.
FleasionPreparation PrepareFleasion(const RuntimeConfig& config,
                                   const Environment& environment,
                                   const RuntimePaths& paths);

}  // namespace mocktail::runtime

#endif
