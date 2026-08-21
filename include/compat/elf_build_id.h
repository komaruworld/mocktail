#ifndef MOCKTAIL_COMPAT_ELF_BUILD_ID_H_
#define MOCKTAIL_COMPAT_ELF_BUILD_ID_H_

#include <string>
#include <string_view>

namespace mocktail::compat {

struct BuildIdResult {
  std::string build_id;
  std::string error;

  explicit operator bool() const noexcept { return error.empty(); }
};

// Reads the GNU build ID through elfutils libelf. The returned identifier is
// lowercase hexadecimal and is empty only when an error is reported.
BuildIdResult ReadElfBuildId(const std::string& path);

bool IsValidBuildId(std::string_view build_id) noexcept;

}  // namespace mocktail::compat

#endif  // MOCKTAIL_COMPAT_ELF_BUILD_ID_H_
