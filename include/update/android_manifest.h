#ifndef MOCKTAIL_UPDATE_ANDROID_MANIFEST_H_
#define MOCKTAIL_UPDATE_ANDROID_MANIFEST_H_

#include <cstdint>
#include <string>
#include <string_view>

namespace mocktail::update {

struct AndroidManifestIdentity {
  std::string package_name;
  std::string version_name;
  std::string split_name;
  std::uint64_t version_code = 0;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

AndroidManifestIdentity ParseAndroidManifest(std::string_view binary_xml);

}  // namespace mocktail::update

#endif  // MOCKTAIL_UPDATE_ANDROID_MANIFEST_H_
