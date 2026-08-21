#include "update/android_manifest.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace mocktail::update {
namespace {

constexpr std::uint16_t kStringPoolChunk = 0x0001;
constexpr std::uint16_t kXmlChunk = 0x0003;
constexpr std::uint16_t kStartElementChunk = 0x0102;
constexpr std::uint32_t kUtf8Flag = 0x00000100;
constexpr std::uint32_t kNoIndex = 0xffffffffU;
constexpr std::uint8_t kTypeString = 0x03;

class Bytes final {
 public:
  explicit Bytes(std::string_view bytes) : bytes_(bytes) {}

  bool U16(std::size_t offset, std::uint16_t* value) const {
    if (offset > bytes_.size() || bytes_.size() - offset < 2) return false;
    *value = static_cast<unsigned char>(bytes_[offset]) |
             static_cast<std::uint16_t>(
                 static_cast<unsigned char>(bytes_[offset + 1]))
                 << 8U;
    return true;
  }

  bool U32(std::size_t offset, std::uint32_t* value) const {
    if (offset > bytes_.size() || bytes_.size() - offset < 4) return false;
    *value = static_cast<unsigned char>(bytes_[offset]) |
             static_cast<std::uint32_t>(
                 static_cast<unsigned char>(bytes_[offset + 1]))
                 << 8U |
             static_cast<std::uint32_t>(
                 static_cast<unsigned char>(bytes_[offset + 2]))
                 << 16U |
             static_cast<std::uint32_t>(
                 static_cast<unsigned char>(bytes_[offset + 3]))
                 << 24U;
    return true;
  }

  std::string_view Slice(std::size_t offset, std::size_t size) const {
    if (offset > bytes_.size() || size > bytes_.size() - offset) return {};
    return bytes_.substr(offset, size);
  }

  std::size_t size() const { return bytes_.size(); }

 private:
  std::string_view bytes_;
};

struct Chunk {
  std::uint16_t type = 0;
  std::uint16_t header_size = 0;
  std::uint32_t size = 0;
};

bool ReadChunk(const Bytes& bytes, std::size_t offset, Chunk* chunk) {
  return bytes.U16(offset, &chunk->type) &&
         bytes.U16(offset + 2, &chunk->header_size) &&
         bytes.U32(offset + 4, &chunk->size) && chunk->header_size >= 8 &&
         chunk->size >= chunk->header_size && offset <= bytes.size() &&
         chunk->size <= bytes.size() - offset;
}

bool ReadUtf8Length(std::string_view bytes, std::size_t* offset,
                    std::size_t* length) {
  if (*offset >= bytes.size()) return false;
  const unsigned char first = bytes[(*offset)++];
  if ((first & 0x80U) == 0) {
    *length = first;
    return true;
  }
  if (*offset >= bytes.size()) return false;
  *length = (static_cast<std::size_t>(first & 0x7fU) << 8U) |
            static_cast<unsigned char>(bytes[(*offset)++]);
  return true;
}

bool ReadUtf16Length(std::string_view bytes, std::size_t* offset,
                     std::size_t* length) {
  if (*offset > bytes.size() || bytes.size() - *offset < 2) return false;
  const std::uint16_t first =
      static_cast<unsigned char>(bytes[*offset]) |
      static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[*offset + 1]))
          << 8U;
  *offset += 2;
  if ((first & 0x8000U) == 0) {
    *length = first;
    return true;
  }
  if (*offset > bytes.size() || bytes.size() - *offset < 2) return false;
  const std::uint16_t second =
      static_cast<unsigned char>(bytes[*offset]) |
      static_cast<std::uint16_t>(static_cast<unsigned char>(bytes[*offset + 1]))
          << 8U;
  *offset += 2;
  *length = (static_cast<std::size_t>(first & 0x7fffU) << 16U) | second;
  return true;
}

void AppendUtf8(std::uint32_t point, std::string* output) {
  if (point <= 0x7fU) {
    output->push_back(static_cast<char>(point));
  } else if (point <= 0x7ffU) {
    output->push_back(static_cast<char>(0xc0U | (point >> 6U)));
    output->push_back(static_cast<char>(0x80U | (point & 0x3fU)));
  } else {
    output->push_back(static_cast<char>(0xe0U | (point >> 12U)));
    output->push_back(static_cast<char>(0x80U | ((point >> 6U) & 0x3fU)));
    output->push_back(static_cast<char>(0x80U | (point & 0x3fU)));
  }
}

struct StringPool {
  std::vector<std::string> strings;
};

bool ParseStringPool(const Bytes& bytes, std::size_t chunk_offset,
                     const Chunk& chunk, StringPool* pool, std::string* error) {
  std::uint32_t count = 0;
  std::uint32_t flags = 0;
  std::uint32_t strings_start = 0;
  if (chunk.header_size < 28 || !bytes.U32(chunk_offset + 8, &count) ||
      count > 1000000 || !bytes.U32(chunk_offset + 16, &flags) ||
      !bytes.U32(chunk_offset + 20, &strings_start) ||
      strings_start >= chunk.size ||
      static_cast<std::uint64_t>(count) * 4U > chunk.size - chunk.header_size) {
    *error = "Android string pool header is invalid";
    return false;
  }
  const bool utf8 = (flags & kUtf8Flag) != 0;
  pool->strings.clear();
  pool->strings.reserve(count);
  for (std::uint32_t index = 0; index < count; ++index) {
    std::uint32_t relative = 0;
    if (!bytes.U32(chunk_offset + chunk.header_size + index * 4U, &relative) ||
        relative >= chunk.size - strings_start) {
      *error = "Android string pool offset is invalid";
      return false;
    }
    const std::string_view storage =
        bytes.Slice(chunk_offset + strings_start + relative,
                    chunk.size - strings_start - relative);
    std::size_t cursor = 0;
    std::size_t character_length = 0;
    std::size_t byte_length = 0;
    std::string value;
    if (utf8) {
      if (!ReadUtf8Length(storage, &cursor, &character_length) ||
          !ReadUtf8Length(storage, &cursor, &byte_length) ||
          cursor > storage.size() || byte_length >= storage.size() - cursor ||
          storage[cursor + byte_length] != '\0') {
        *error = "Android UTF-8 string is malformed";
        return false;
      }
      value.assign(storage.substr(cursor, byte_length));
    } else {
      if (!ReadUtf16Length(storage, &cursor, &character_length) ||
          character_length > (storage.size() - cursor) / 2U) {
        *error = "Android UTF-16 string is malformed";
        return false;
      }
      for (std::size_t character = 0; character < character_length;
           ++character) {
        const std::size_t position = cursor + character * 2U;
        const std::uint16_t point =
            static_cast<unsigned char>(storage[position]) |
            static_cast<std::uint16_t>(
                static_cast<unsigned char>(storage[position + 1]))
                << 8U;
        if (point >= 0xd800U && point <= 0xdfffU) {
          *error = "Android manifest contains unsupported UTF-16 surrogate";
          return false;
        }
        AppendUtf8(point, &value);
      }
    }
    pool->strings.push_back(std::move(value));
  }
  return true;
}

std::optional<std::string> PoolString(const StringPool& pool,
                                      std::uint32_t index) {
  if (index == kNoIndex) return std::nullopt;
  if (index >= pool.strings.size()) return std::nullopt;
  return pool.strings[index];
}

}  // namespace

AndroidManifestIdentity ParseAndroidManifest(std::string_view binary_xml) {
  AndroidManifestIdentity result;
  const Bytes bytes(binary_xml);
  Chunk xml;
  if (!ReadChunk(bytes, 0, &xml) || xml.type != kXmlChunk ||
      xml.size != bytes.size()) {
    result.error = "AndroidManifest.xml is not valid binary XML";
    return result;
  }
  StringPool pool;
  bool has_pool = false;
  for (std::size_t offset = xml.header_size; offset < xml.size;) {
    Chunk chunk;
    if (!ReadChunk(bytes, offset, &chunk)) {
      result.error = "AndroidManifest.xml contains an invalid chunk";
      return result;
    }
    if (chunk.type == kStringPoolChunk) {
      if (has_pool ||
          !ParseStringPool(bytes, offset, chunk, &pool, &result.error)) {
        if (result.error.empty())
          result.error = "duplicate Android string pool";
        return result;
      }
      has_pool = true;
    } else if (chunk.type == kStartElementChunk && has_pool) {
      std::uint32_t tag_index = 0;
      std::uint16_t attribute_start = 0;
      std::uint16_t attribute_size = 0;
      std::uint16_t attribute_count = 0;
      // RES_XML_START_ELEMENT_TYPE uses a 16-byte ResXMLTree_node header;
      // its 20-byte ResXMLTree_attrExt follows that header.
      if (chunk.header_size != 16 || chunk.size < 36 ||
          !bytes.U32(offset + 20, &tag_index) ||
          !bytes.U16(offset + 24, &attribute_start) ||
          !bytes.U16(offset + 26, &attribute_size) ||
          !bytes.U16(offset + 28, &attribute_count) || attribute_size < 20) {
        result.error = "Android XML start element is malformed";
        return result;
      }
      const auto tag = PoolString(pool, tag_index);
      if (!tag.has_value() || *tag != "manifest") {
        offset += chunk.size;
        continue;
      }
      const std::size_t attributes = offset + 16U + attribute_start;
      if (attributes > offset + chunk.size ||
          static_cast<std::size_t>(attribute_count) * attribute_size >
              offset + chunk.size - attributes) {
        result.error = "Android manifest attributes exceed their chunk";
        return result;
      }
      for (std::uint16_t index = 0; index < attribute_count; ++index) {
        const std::size_t attribute = attributes + index * attribute_size;
        std::uint32_t name_index = 0;
        std::uint32_t raw_index = 0;
        std::uint32_t typed_data = 0;
        if (!bytes.U32(attribute + 4, &name_index) ||
            !bytes.U32(attribute + 8, &raw_index) ||
            !bytes.U32(attribute + 16, &typed_data)) {
          result.error = "Android manifest attribute is truncated";
          return result;
        }
        const auto name = PoolString(pool, name_index);
        if (!name.has_value()) continue;
        const std::uint8_t data_type =
            static_cast<unsigned char>(binary_xml[attribute + 15]);
        std::optional<std::string> value = PoolString(pool, raw_index);
        if (!value.has_value() && data_type == kTypeString) {
          value = PoolString(pool, typed_data);
        }
        if (*name == "package" && value.has_value()) {
          result.package_name = *value;
        } else if (*name == "versionName" && value.has_value()) {
          result.version_name = *value;
        } else if (*name == "split" && value.has_value()) {
          result.split_name = *value;
        } else if (*name == "versionCode" && data_type >= 0x10 &&
                   data_type <= 0x1f) {
          result.version_code = typed_data;
        }
      }
      if (result.package_name.empty() || result.version_code == 0) {
        result.error = "Android manifest has no package or versionCode";
      }
      return result;
    }
    offset += chunk.size;
  }
  result.error = "Android manifest element is missing";
  return result;
}

}  // namespace mocktail::update
