#include "update/zip_archive.h"

#include <fcntl.h>
#include <minizip/unzip.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <vector>

namespace mocktail::update {
namespace {

struct ArchiveCloser {
  void operator()(void* archive) const {
    if (archive != nullptr) unzClose(archive);
  }
};

using Archive = std::unique_ptr<void, ArchiveCloser>;

Archive Open(const std::filesystem::path& path, std::string* error) {
  std::error_code filesystem_error;
  const std::filesystem::file_status status =
      std::filesystem::symlink_status(path, filesystem_error);
  if (filesystem_error || !std::filesystem::is_regular_file(status) ||
      std::filesystem::is_symlink(status)) {
    *error = "ZIP archive is not a regular file: " + path.string();
    return {};
  }
  Archive archive(unzOpen64(path.c_str()));
  if (!archive) *error = "cannot open ZIP archive: " + path.string();
  return archive;
}

bool CurrentInfo(void* archive, ZipEntry* entry, std::string* error) {
  unz_file_info64 info = {};
  if (unzGetCurrentFileInfo64(archive, &info, nullptr, 0, nullptr, 0, nullptr,
                              0) != UNZ_OK ||
      info.size_filename == 0 || info.size_filename > 64U * 1024U) {
    *error = "cannot read ZIP entry metadata";
    return false;
  }
  std::vector<char> name(static_cast<std::size_t>(info.size_filename) + 1U);
  if (unzGetCurrentFileInfo64(archive, &info, name.data(), name.size(), nullptr,
                              0, nullptr, 0) != UNZ_OK) {
    *error = "cannot read ZIP entry name";
    return false;
  }
  entry->name.assign(name.data(), static_cast<std::size_t>(info.size_filename));
  entry->uncompressed_size = info.uncompressed_size;
  entry->directory = !entry->name.empty() && entry->name.back() == '/';
  const mode_t unix_mode =
      static_cast<mode_t>((info.external_fa >> 16U) & 0xffffU);
  entry->symbolic_link = (unix_mode & S_IFMT) == S_IFLNK;
  return true;
}

bool First(void* archive, std::string* error) {
  const int status = unzGoToFirstFile(archive);
  if (status == UNZ_END_OF_LIST_OF_FILE) return false;
  if (status != UNZ_OK) *error = "cannot enumerate ZIP archive";
  return status == UNZ_OK;
}

bool WriteCurrent(void* archive, int descriptor, std::size_t maximum,
                  std::size_t* written, std::string* error) {
  if (unzOpenCurrentFile(archive) != UNZ_OK) {
    *error = "cannot open compressed ZIP entry";
    return false;
  }
  std::array<char, 128U * 1024U> buffer{};
  bool success = true;
  while (success) {
    const int bytes = unzReadCurrentFile(archive, buffer.data(), buffer.size());
    if (bytes == 0) break;
    if (bytes < 0) {
      *error = "cannot decompress ZIP entry";
      success = false;
      break;
    }
    const std::size_t chunk = static_cast<std::size_t>(bytes);
    if (chunk > maximum || *written > maximum - chunk) {
      *error = "decompressed ZIP data exceeds its size limit";
      success = false;
      break;
    }
    std::size_t offset = 0;
    while (offset < chunk) {
      const ssize_t output =
          write(descriptor, buffer.data() + offset, chunk - offset);
      if (output < 0) {
        if (errno == EINTR) continue;
        *error = "cannot write decompressed ZIP entry: " +
                 std::string(std::strerror(errno));
        success = false;
        break;
      }
      offset += static_cast<std::size_t>(output);
    }
    *written += chunk;
  }
  const int close_status = unzCloseCurrentFile(archive);
  if (success && close_status != UNZ_OK) {
    *error = "ZIP entry failed its CRC check";
    success = false;
  }
  return success;
}

bool Locate(void* archive, std::string_view entry, ZipEntry* metadata,
            std::string* error) {
  if (entry.empty() || entry.find('\0') != std::string_view::npos ||
      unzLocateFile(archive, std::string(entry).c_str(), 1) != UNZ_OK) {
    *error = "ZIP entry is missing: " + std::string(entry);
    return false;
  }
  return CurrentInfo(archive, metadata, error);
}

}  // namespace

bool IsSafeArchivePath(std::string_view path) {
  if (path.empty() || path.front() == '/' || path.find('\0') != path.npos ||
      path.find('\\') != path.npos) {
    return false;
  }
  std::size_t begin = 0;
  while (begin < path.size()) {
    std::size_t end = path.find('/', begin);
    if (end == path.npos) end = path.size();
    const std::string_view component = path.substr(begin, end - begin);
    if (component.empty() || component == "." || component == "..") {
      return end == path.size() && component.empty();
    }
    begin = end + 1;
  }
  return true;
}

ZipListResult ListZipEntries(const std::filesystem::path& archive_path,
                             std::size_t maximum_entries) {
  ZipListResult result;
  Archive archive = Open(archive_path, &result.error);
  if (!archive) return result;
  unz_global_info64 global = {};
  if (unzGetGlobalInfo64(archive.get(), &global) != UNZ_OK ||
      global.number_entry > maximum_entries) {
    result.error = "ZIP archive has an invalid or excessive entry count";
    return result;
  }
  if (global.number_entry == 0) return result;
  if (!First(archive.get(), &result.error)) return result;
  for (std::uint64_t index = 0; index < global.number_entry; ++index) {
    ZipEntry entry;
    if (!CurrentInfo(archive.get(), &entry, &result.error)) return result;
    if (!IsSafeArchivePath(entry.name) || entry.symbolic_link) {
      result.error = "ZIP archive contains an unsafe entry: " + entry.name;
      return result;
    }
    result.entries.push_back(std::move(entry));
    if (index + 1 < global.number_entry &&
        unzGoToNextFile(archive.get()) != UNZ_OK) {
      result.error = "ZIP archive ended before its declared entry count";
      return result;
    }
  }
  return result;
}

ZipReadResult ReadZipEntry(const std::filesystem::path& archive_path,
                           std::string_view entry, std::size_t maximum_bytes) {
  ZipReadResult result;
  Archive archive = Open(archive_path, &result.error);
  if (!archive) return result;
  ZipEntry metadata;
  if (!Locate(archive.get(), entry, &metadata, &result.error) ||
      metadata.directory || metadata.symbolic_link ||
      metadata.uncompressed_size > maximum_bytes) {
    if (result.error.empty())
      result.error = "ZIP entry is invalid or too large";
    return result;
  }
  if (unzOpenCurrentFile(archive.get()) != UNZ_OK) {
    result.error = "cannot open compressed ZIP entry";
    return result;
  }
  result.bytes.reserve(static_cast<std::size_t>(metadata.uncompressed_size));
  std::array<char, 64U * 1024U> buffer{};
  while (true) {
    const int bytes =
        unzReadCurrentFile(archive.get(), buffer.data(), buffer.size());
    if (bytes == 0) break;
    if (bytes < 0 || static_cast<std::size_t>(bytes) > maximum_bytes ||
        result.bytes.size() > maximum_bytes - static_cast<std::size_t>(bytes)) {
      unzCloseCurrentFile(archive.get());
      result.bytes.clear();
      result.error = bytes < 0 ? "cannot decompress ZIP entry"
                               : "ZIP entry exceeds its size limit";
      return result;
    }
    result.bytes.append(buffer.data(), static_cast<std::size_t>(bytes));
  }
  if (unzCloseCurrentFile(archive.get()) != UNZ_OK) {
    result.bytes.clear();
    result.error = "ZIP entry failed its CRC check";
  }
  return result;
}

bool ExtractZipEntry(const std::filesystem::path& archive_path,
                     std::string_view entry,
                     const std::filesystem::path& destination,
                     std::size_t maximum_bytes, std::string* error) {
  Archive archive = Open(archive_path, error);
  if (!archive) return false;
  ZipEntry metadata;
  if (!Locate(archive.get(), entry, &metadata, error) || metadata.directory ||
      metadata.symbolic_link || metadata.uncompressed_size > maximum_bytes) {
    if (error->empty()) *error = "ZIP entry is invalid or too large";
    return false;
  }
  std::error_code filesystem_error;
  std::filesystem::create_directories(destination.parent_path(),
                                      filesystem_error);
  if (filesystem_error) {
    *error = "cannot create ZIP extraction directory";
    return false;
  }
  const int descriptor =
      open(destination.c_str(),
           O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (descriptor < 0) {
    *error = "cannot create extracted ZIP entry";
    return false;
  }
  std::size_t written = 0;
  const bool extracted =
      WriteCurrent(archive.get(), descriptor, maximum_bytes, &written, error);
  const bool synced = fsync(descriptor) == 0;
  close(descriptor);
  if (!extracted || !synced || written != metadata.uncompressed_size) {
    std::filesystem::remove(destination, filesystem_error);
    if (extracted && (!synced || written != metadata.uncompressed_size)) {
      *error = "extracted ZIP entry is truncated";
    }
    return false;
  }
  return true;
}

bool ExtractZipPrefix(const std::filesystem::path& archive_path,
                      std::string_view prefix,
                      const std::filesystem::path& destination,
                      std::size_t maximum_total_bytes,
                      std::size_t* extracted_files, std::string* error) {
  const ZipListResult listed = ListZipEntries(archive_path);
  if (!listed) {
    *error = listed.error;
    return false;
  }
  Archive archive = Open(archive_path, error);
  if (!archive) return false;
  if (!listed.entries.empty() && !First(archive.get(), error)) return false;
  std::size_t total = 0;
  std::size_t files = 0;
  for (std::size_t index = 0; index < listed.entries.size(); ++index) {
    ZipEntry current;
    if (!CurrentInfo(archive.get(), &current, error)) return false;
    const ZipEntry& entry = listed.entries[index];
    if (current.name != entry.name ||
        current.uncompressed_size != entry.uncompressed_size ||
        current.directory != entry.directory ||
        current.symbolic_link != entry.symbolic_link) {
      *error = "ZIP archive changed during extraction";
      return false;
    }
    const auto advance = [&]() {
      if (index + 1 >= listed.entries.size()) return true;
      if (unzGoToNextFile(archive.get()) == UNZ_OK) return true;
      *error = "ZIP archive ended during extraction";
      return false;
    };
    if (entry.name.substr(0, prefix.size()) != prefix) {
      if (!advance()) return false;
      continue;
    }
    const std::string_view relative(entry.name.data() + prefix.size(),
                                    entry.name.size() - prefix.size());
    if (relative.empty()) {
      if (!advance()) return false;
      continue;
    }
    if (!IsSafeArchivePath(relative) || entry.symbolic_link ||
        entry.uncompressed_size > maximum_total_bytes ||
        total > maximum_total_bytes - entry.uncompressed_size) {
      *error = "ZIP prefix contains an unsafe or oversized entry";
      return false;
    }
    const std::filesystem::path output = destination / std::string(relative);
    std::error_code filesystem_error;
    if (entry.directory) {
      std::filesystem::create_directories(output, filesystem_error);
      if (filesystem_error) {
        *error = "cannot create extracted ZIP directory";
        return false;
      }
    } else {
      std::filesystem::create_directories(output.parent_path(),
                                          filesystem_error);
      if (filesystem_error) {
        *error = "cannot create extracted ZIP directory";
        return false;
      }
      const int descriptor =
          open(output.c_str(),
               O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
      if (descriptor < 0) {
        *error = "cannot create extracted ZIP entry";
        return false;
      }
      std::size_t written = 0;
      const bool extracted = WriteCurrent(
          archive.get(), descriptor,
          static_cast<std::size_t>(entry.uncompressed_size), &written, error);
      close(descriptor);
      if (!extracted || written != entry.uncompressed_size) {
        std::filesystem::remove(output, filesystem_error);
        if (extracted && written != entry.uncompressed_size) {
          *error = "extracted ZIP entry is truncated";
        }
        return false;
      }
      total += static_cast<std::size_t>(entry.uncompressed_size);
      ++files;
    }
    if (!advance()) return false;
  }
  if (extracted_files != nullptr) *extracted_files = files;
  return true;
}

}  // namespace mocktail::update
