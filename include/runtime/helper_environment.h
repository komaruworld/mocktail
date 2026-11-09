#ifndef MOCKTAIL_RUNTIME_HELPER_ENVIRONMENT_H_
#define MOCKTAIL_RUNTIME_HELPER_ENVIRONMENT_H_

#include <cstring>
#include <vector>

namespace mocktail::runtime {

// Helpers do not consume FFlag overrides, which may exceed exec's environment
// string limit. Borrow the other entries without changing the parent process.
inline std::vector<char*> BuildHelperEnvironment(char* const* inherited) {
  constexpr char kOverrides[] = "MOCKTAIL_CLIENT_SETTINGS_OVERRIDES_JSON=";
  std::vector<char*> result;
  for (char* const* entry = inherited; entry != nullptr && *entry != nullptr;
       ++entry) {
    if (std::strncmp(*entry, kOverrides, sizeof(kOverrides) - 1) != 0) {
      result.push_back(*entry);
    }
  }
  result.push_back(nullptr);
  return result;
}

}  // namespace mocktail::runtime

#endif  // MOCKTAIL_RUNTIME_HELPER_ENVIRONMENT_H_
