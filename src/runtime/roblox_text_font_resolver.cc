#include "runtime/roblox_text_font_resolver.h"

#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace mocktail {
namespace runtime {
namespace {

struct FontMapping {
  std::string file;
  float size_scale = 0.795F;
};

using FontMappings = std::unordered_map<int32_t, FontMapping>;

std::mutex g_catalog_mutex;
std::unordered_map<std::string, FontMappings> g_catalogs;

bool IsSafeFontFileName(const std::string& value) {
  if (value.empty()) {
    return false;
  }
  const std::filesystem::path path(value);
  return !path.is_absolute() && !path.has_parent_path() &&
         path.filename() == path;
}

bool IsRegularFile(const std::filesystem::path& path) {
  std::error_code error;
  return std::filesystem::is_regular_file(path, error) && !error;
}

FontMappings LoadMappings(const std::filesystem::path& asset_content_root) {
  FontMappings mappings;
  const std::filesystem::path mapping_file =
      asset_content_root.parent_path() / "android/fonts/font-mappings.json";
  std::ifstream input(mapping_file, std::ios::binary);
  if (!input) {
    return mappings;
  }
  const nlohmann::json document = nlohmann::json::parse(input, nullptr, false);
  if (document.is_discarded() || !document.is_array()) {
    return mappings;
  }
  for (const nlohmann::json& item : document) {
    if (!item.is_object()) {
      continue;
    }
    const auto enum_value = item.find("enum");
    const auto font_value = item.find("font");
    const auto ratio_value = item.find("fromRbxFontRatio");
    if (enum_value == item.end() || !enum_value->is_number_integer() ||
        font_value == item.end() || !font_value->is_string() ||
        ratio_value == item.end() || !ratio_value->is_number()) {
      continue;
    }
    const int64_t encoded_enum = enum_value->get<int64_t>();
    const double encoded_ratio = ratio_value->get<double>();
    const std::string encoded_font = font_value->get<std::string>();
    if (encoded_enum < 0 || encoded_enum > INT32_MAX ||
        !std::isfinite(encoded_ratio) || encoded_ratio <= 0.0 ||
        encoded_ratio > 4.0 || !IsSafeFontFileName(encoded_font)) {
      continue;
    }
    mappings[static_cast<int32_t>(encoded_enum)] = {
        encoded_font, static_cast<float>(encoded_ratio)};
  }
  return mappings;
}

std::optional<FontMapping> MappingFor(
    int32_t font, const std::filesystem::path& asset_content_root) {
  const std::string key = asset_content_root.lexically_normal().string();
  std::lock_guard<std::mutex> lock(g_catalog_mutex);
  auto catalog = g_catalogs.find(key);
  if (catalog == g_catalogs.end()) {
    catalog = g_catalogs.emplace(key, LoadMappings(asset_content_root)).first;
  }
  const auto mapped = catalog->second.find(font);
  return mapped == catalog->second.end()
             ? std::optional<FontMapping>()
             : std::optional<FontMapping>(mapped->second);
}

std::string LegacyFontName(int32_t font) {
  if (font == 4) {
    return "SourceSansPro-Bold.ttf";
  }
  if (font == 5) {
    return "SourceSansPro-Light.ttf";
  }
  return "SourceSansPro-Regular.ttf";
}

}  // namespace

RobloxTextFontSelection ResolveRobloxTextFont(
    int32_t font, const std::string& asset_content_root) {
  RobloxTextFontSelection selection;
  if (asset_content_root.empty()) {
    return selection;
  }

  const std::filesystem::path content(asset_content_root);
  const std::optional<FontMapping> mapped = MappingFor(font, content);
  if (mapped.has_value()) {
    const std::filesystem::path candidate = content / "fonts" / mapped->file;
    if (IsRegularFile(candidate)) {
      selection.primary_file = candidate.lexically_normal().string();
      selection.size_scale = mapped->size_scale;
      selection.exact_mapping = true;
      return selection;
    }
  }

  const std::filesystem::path legacy =
      content.parent_path() / "fonts" / LegacyFontName(font);
  if (IsRegularFile(legacy)) {
    selection.primary_file = legacy.lexically_normal().string();
  }
  return selection;
}

RobloxTextFontSelection ResolveRobloxTextFont(int32_t font) {
  const char* content = std::getenv("MOCKTAIL_ASSET_PATH");
  return ResolveRobloxTextFont(
      font, content != nullptr && content[0] != '\0' ? content : "");
}

}  // namespace runtime
}  // namespace mocktail
