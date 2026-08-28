#ifndef MOCKTAIL_RUNTIME_ROBLOX_TEXT_FONT_RESOLVER_H_
#define MOCKTAIL_RUNTIME_ROBLOX_TEXT_FONT_RESOLVER_H_

#include <cstdint>
#include <string>

namespace mocktail {
namespace runtime {

// Android's RbxKeyboard resolves the Roblox Font enum through
// assets/android/fonts/font-mappings.json. The selected file lives below
// assets/content/fonts while legacy SourceSansPro faces live in assets/fonts.
struct RobloxTextFontSelection {
  std::string primary_file;
  float size_scale = 0.795F;
  bool exact_mapping = false;
};

RobloxTextFontSelection ResolveRobloxTextFont(
    int32_t font, const std::string& asset_content_root);
RobloxTextFontSelection ResolveRobloxTextFont(int32_t font);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_ROBLOX_TEXT_FONT_RESOLVER_H_
