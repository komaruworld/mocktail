#include "update/update_config.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <yaml.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <string>
#include <unordered_set>

namespace mocktail::update {
namespace {

constexpr std::size_t kMaximumConfigBytes = 1024U * 1024U;

std::string Scalar(const yaml_node_t* node) {
  if (node == nullptr || node->type != YAML_SCALAR_NODE) {
    return {};
  }
  return std::string(reinterpret_cast<const char*>(node->data.scalar.value),
                     node->data.scalar.length);
}

bool ParseBoolean(const yaml_node_t* node, bool* value) {
  const std::string text = Scalar(node);
  if (text == "true") {
    *value = true;
    return true;
  }
  if (text == "false") {
    *value = false;
    return true;
  }
  return false;
}

bool ParseVersion(const yaml_node_t* node, std::string* value) {
  const std::string text = Scalar(node);
  const bool valid_character = std::all_of(
      text.begin(), text.end(), [](unsigned char character) {
        return std::isalnum(character) || character == '.' ||
               character == '_' || character == '-';
      });
  if (text.empty() || text.size() > 128 ||
      !std::isdigit(static_cast<unsigned char>(text.front())) ||
      !valid_character) {
    return false;
  }
  *value = text;
  return true;
}

bool ReadFile(const std::filesystem::path& path, std::string* contents,
              bool* loaded, std::string* error) {
  const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    if (errno == ENOENT) {
      *loaded = false;
      return true;
    }
    *error = "cannot open update configuration: " + path.string();
    return false;
  }
  struct stat metadata = {};
  if (fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
      metadata.st_size < 0 ||
      static_cast<std::uintmax_t>(metadata.st_size) > kMaximumConfigBytes) {
    close(descriptor);
    *error = "update configuration must be a regular file no larger than 1 MiB";
    return false;
  }
  contents->clear();
  contents->reserve(static_cast<std::size_t>(metadata.st_size));
  std::array<char, 16U * 1024U> buffer{};
  while (true) {
    const ssize_t read_bytes = read(descriptor, buffer.data(), buffer.size());
    if (read_bytes == 0) {
      break;
    }
    if (read_bytes < 0) {
      if (errno == EINTR) {
        continue;
      }
      close(descriptor);
      *error = "cannot read update configuration: " + path.string();
      return false;
    }
    contents->append(buffer.data(), static_cast<std::size_t>(read_bytes));
  }
  close(descriptor);
  *loaded = true;
  return true;
}

}  // namespace

UpdateConfigResult LoadUpdateConfig(const std::filesystem::path& path) {
  UpdateConfigResult result;
  std::string contents;
  if (!ReadFile(path, &contents, &result.file_loaded, &result.error) ||
      !result.file_loaded) {
    return result;
  }

  yaml_parser_t parser;
  yaml_document_t document;
  if (!yaml_parser_initialize(&parser)) {
    result.error = "cannot initialise YAML parser";
    return result;
  }
  yaml_parser_set_input_string(
      &parser, reinterpret_cast<const unsigned char*>(contents.data()),
      contents.size());
  const bool parsed = yaml_parser_load(&parser, &document) != 0;
  yaml_parser_delete(&parser);
  if (!parsed) {
    result.error = "cannot parse update configuration";
    return result;
  }

  const yaml_node_t* root = yaml_document_get_root_node(&document);
  if (root == nullptr || root->type != YAML_MAPPING_NODE) {
    yaml_document_delete(&document);
    result.error = "configuration root must be a mapping";
    return result;
  }
  const yaml_node_t* updates = nullptr;
  for (yaml_node_pair_t* pair = root->data.mapping.pairs.start;
       pair != root->data.mapping.pairs.top; ++pair) {
    const std::string key =
        Scalar(yaml_document_get_node(&document, pair->key));
    if (key == "version") {
      const std::string version =
          Scalar(yaml_document_get_node(&document, pair->value));
      if (version != "1") {
        result.error = "only configuration version 1 is supported";
        break;
      }
    } else if (key == "updates") {
      updates = yaml_document_get_node(&document, pair->value);
    }
  }
  if (!result.error.empty() || updates == nullptr) {
    yaml_document_delete(&document);
    return result;
  }
  if (updates->type != YAML_MAPPING_NODE) {
    yaml_document_delete(&document);
    result.error = "updates must be a mapping";
    return result;
  }

  std::unordered_set<std::string> keys;
  for (yaml_node_pair_t* pair = updates->data.mapping.pairs.start;
       pair != updates->data.mapping.pairs.top; ++pair) {
    const std::string key =
        Scalar(yaml_document_get_node(&document, pair->key));
    const yaml_node_t* value = yaml_document_get_node(&document, pair->value);
    if (key.empty() || !keys.insert(key).second) {
      result.error = "updates contains an empty or duplicate key";
      break;
    }
    if (key == "automatic") {
      if (!ParseBoolean(value, &result.config.automatic)) {
        result.error = "updates.automatic must be true or false";
        break;
      }
    } else if (key == "testing_latest_only") {
      bool ignored = false;
      if (!ParseBoolean(value, &ignored)) {
        result.error = "updates.testing_latest_only must be true or false";
        break;
      }
      result.warnings.emplace_back(
          "updates.testing_latest_only is no longer supported and is ignored");
    } else if (key == "launch_after_update") {
      if (!ParseBoolean(value, &result.config.launch_after_update)) {
        result.error = "updates.launch_after_update must be true or false";
        break;
      }
    } else if (key == "source") {
      result.config.source = Scalar(value);
      if (result.config.source != "apk-pure") {
        result.error = "updates.source must be apk-pure in the native updater";
        break;
      }
    } else if (key == "version") {
      if (!ParseVersion(value, &result.config.version)) {
        result.error = "updates.version must be a Roblox version name";
        break;
      }
    } else {
      result.error = "unknown updates key: " + key;
      break;
    }
  }
  yaml_document_delete(&document);
  return result;
}

}  // namespace mocktail::update
