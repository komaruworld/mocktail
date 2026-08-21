#include "services/client_settings_service.h"

#define JSON_NOEXCEPTION 1
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <system_error>
#include <utility>

#include "runtime/runtime_paths.h"

namespace mocktail {
namespace services {
namespace {

constexpr const char kEmptyDefaults[] = "{\"applicationSettings\":{}}";
constexpr std::uintmax_t kMaximumFflagsFileBytes = 64U * 1024U;

class ScopedFileDescriptor final {
 public:
  explicit ScopedFileDescriptor(int descriptor) : descriptor_(descriptor) {}
  ~ScopedFileDescriptor() { close(descriptor_); }

  ScopedFileDescriptor(const ScopedFileDescriptor&) = delete;
  ScopedFileDescriptor& operator=(const ScopedFileDescriptor&) = delete;

  int get() const { return descriptor_; }

 private:
  int descriptor_;
};

bool IsSupportedFflagValue(const nlohmann::json& value) {
  return value.is_string() || value.is_boolean() ||
         value.is_number_integer() || value.is_number_unsigned();
}

std::string FflagValueString(const nlohmann::json& value) {
  if (value.is_string()) {
    return value.get<std::string>();
  }
  if (value.is_boolean()) {
    return value.get<bool>() ? "True" : "False";
  }
  if (value.is_number_unsigned()) {
    return std::to_string(value.get<std::uint64_t>());
  }
  return std::to_string(value.get<std::int64_t>());
}

std::string ReadFile(const std::filesystem::path& path) {
  if (path.empty()) {
    return {};
  }
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    return {};
  }
  return std::string((std::istreambuf_iterator<char>(file)),
                     std::istreambuf_iterator<char>());
}

bool AtomicWriteFile(const std::filesystem::path& path,
                     const std::string& content, std::string* error_message) {
  if (path.empty()) {
    if (error_message != nullptr) {
      *error_message = "client settings cache path is empty";
    }
    return false;
  }
  std::error_code directory_error;
  const std::filesystem::path parent = path.parent_path().empty()
                                           ? std::filesystem::path(".")
                                           : path.parent_path();
  if (!mocktail::runtime::RuntimePaths::EnsureDirectory(parent,
                                                        &directory_error)) {
    if (error_message != nullptr) {
      *error_message = "could not create client settings cache directory: " +
                       directory_error.message();
    }
    return false;
  }

  const std::filesystem::path temporary =
      path.string() + ".tmp." + std::to_string(getpid());
  const int fd = open(temporary.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
  if (fd < 0) {
    if (error_message != nullptr) {
      *error_message = "could not create client settings temporary file";
    }
    return false;
  }

  bool ok = true;
  const char* cursor = content.data();
  size_t remaining = content.size();
  while (remaining != 0) {
    const ssize_t written = write(fd, cursor, remaining);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      ok = false;
      break;
    }
    cursor += written;
    remaining -= static_cast<size_t>(written);
  }
  if (ok && fsync(fd) != 0) {
    ok = false;
  }
  if (close(fd) != 0) {
    ok = false;
  }
  if (ok && rename(temporary.c_str(), path.c_str()) != 0) {
    ok = false;
  }
  if (!ok) {
    unlink(temporary.c_str());
    if (error_message != nullptr) {
      *error_message = "could not commit client settings cache";
    }
  }
  return ok;
}

ClientSettingsResult FileResult(const std::filesystem::path& path,
                                ClientSettingsSource source) {
  ClientSettingsResult result;
  result.json = ReadFile(path);
  result.source = source;
  result.valid_json = ClientSettingsService::IsValidJson(result.json);
  return result;
}

}  // namespace

FflagsMergeResult LoadAndMergeFflagsFile(const std::filesystem::path& path,
                                         std::string_view base_json) {
  FflagsMergeResult result;
  nlohmann::json merged = nlohmann::json::parse(
      base_json.empty() ? std::string_view("{}") : base_json, nullptr, false,
      true);
  if (merged.is_discarded() || !merged.is_object()) {
    result.json = std::string(base_json);
    result.error = "base client-settings overrides must be a JSON object";
    return result;
  }
  result.json = merged.dump();

  if (path.empty()) {
    return result;
  }
  const int descriptor =
      open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
  if (descriptor < 0) {
    if (errno != ENOENT) {
      result.error = "cannot safely open fflags file";
    }
    return result;
  }
  const ScopedFileDescriptor file(descriptor);

  struct stat metadata = {};
  if (fstat(file.get(), &metadata) != 0) {
    result.error = "cannot inspect fflags file";
    return result;
  }
  if (!S_ISREG(metadata.st_mode)) {
    result.error = "fflags file is not a regular file";
    return result;
  }
  if (metadata.st_size < 0 ||
      static_cast<std::uintmax_t>(metadata.st_size) >
          kMaximumFflagsFileBytes) {
    result.error = "fflags file exceeds the 64 KiB read limit";
    return result;
  }

  std::string bytes;
  bytes.reserve(static_cast<std::size_t>(metadata.st_size));
  std::array<char, 4096> buffer = {};
  while (true) {
    const ssize_t read_count = read(file.get(), buffer.data(), buffer.size());
    if (read_count == 0) {
      break;
    }
    if (read_count < 0) {
      if (errno == EINTR) {
        continue;
      }
      result.error = "cannot read fflags file";
      return result;
    }
    const std::size_t count = static_cast<std::size_t>(read_count);
    if (bytes.size() >
        static_cast<std::size_t>(kMaximumFflagsFileBytes) - count) {
      result.error = "fflags file exceeds the 64 KiB read limit";
      return result;
    }
    bytes.append(buffer.data(), count);
  }

  nlohmann::json file_values =
      nlohmann::json::parse(bytes, nullptr, false, true);
  if (file_values.is_discarded() || !file_values.is_object()) {
    result.error = "fflags file must contain a JSON object";
    return result;
  }
  for (const auto& [key, value] : file_values.items()) {
    if (key.empty()) {
      result.error = "fflags file contains an invalid key";
      return result;
    }
    if (!IsSupportedFflagValue(value)) {
      result.error =
          "fflags file values must be strings, booleans, or integers";
      return result;
    }
  }

  for (const auto& [key, value] : file_values.items()) {
    if (!merged.contains(key)) {
      merged[key] = FflagValueString(value);
      ++result.count;
    }
  }
  result.json = merged.dump();
  result.loaded = true;
  return result;
}

ClientSettingsResult ClientSettingsService::Resolve(
    const ClientSettingsOptions& options) {
  if (!options.explicit_json.empty()) {
    return {options.explicit_json,
            ClientSettingsSource::kExplicitJson,
            IsValidJson(options.explicit_json),
            false,
            {}};
  }

  if (!options.explicit_file.empty()) {
    ClientSettingsResult result =
        FileResult(options.explicit_file, ClientSettingsSource::kExplicitFile);
    if (!result.json.empty()) {
      return result;
    }
  }

  if (!options.fetch && options.use_bundled) {
    ClientSettingsResult result =
        FileResult(options.bundled_file, ClientSettingsSource::kBundledFile);
    if (!result.json.empty()) {
      return result;
    }
  }

  if (options.sober_mode && !options.fetch) {
    const std::string defaults = SafeDefaultsJson();
    return {defaults, ClientSettingsSource::kSafeDefaults, true, false, {}};
  }

  std::string update_error;
  if (options.auto_update && !options.cache_file.empty()) {
    HttpRequest request;
    request.url =
        options.url.empty() ? DefaultUrl(options.application) : options.url;
    request.timeout_ms = 20000;
    request.maximum_body_bytes = 16 * 1024 * 1024;
    request.headers.push_back("Accept: application/json");
    HttpResponse response = http_client_.Get(request);
    if (response.transport_ok && response.status_code >= 200 &&
        response.status_code < 300 && !response.body.empty() &&
        IsValidJson(response.body)) {
      const std::string cached = ReadFile(options.cache_file);
      bool updated = false;
      if (cached != response.body) {
        updated =
            AtomicWriteFile(options.cache_file, response.body, &update_error);
      }
      if (cached == response.body || updated) {
        return {response.body, ClientSettingsSource::kDownloaded, true, updated,
                update_error};
      }
    } else if (!response.transport_ok) {
      update_error = std::move(response.error);
    } else if (response.status_code < 200 || response.status_code >= 300) {
      update_error = "client settings server returned HTTP " +
                     std::to_string(response.status_code);
    } else {
      update_error = "client settings response was empty or invalid JSON";
    }
  }

  ClientSettingsResult cached =
      FileResult(options.cache_file, ClientSettingsSource::kCache);
  if (!cached.json.empty()) {
    cached.error = std::move(update_error);
    return cached;
  }

  return {kEmptyDefaults, ClientSettingsSource::kEmptyDefaults, true, false,
          std::move(update_error)};
}

std::string ClientSettingsService::SafeDefaultsJson() {
  return R"json({
    "applicationSettings": {
      "BeginScheduledFlagFetch3": "False",
      "BeginScheduledFlagFetch5": "false",
      "RetryFlagPrefetchOnBackgroundFailure": "false",
      "FFlagEnableVersionCheckFromClientSettingsCDN": "False",
      "FIntScheduledFlagFetchPeriodMinutes": "1000000",
      "FIntScheduledFlagFetchPeriodFlexMinutes": "0",
      "DFFlagFetchAndWriteFlagsAfterSuccessfulCachedFlagsLoad": "False",
      "DFFlagWriteFlagCacheAfterDynamicFetch": "False",
      "DFFlagWriteFlagCacheAfterFlagFetch": "False",
      "DFFlagWriteFlagCacheAfterFlagFetch2": "False",
      "DFFlagEnableAppPlatformQoSEmergencyOnStartup3": "False",
      "DFFlagEnableAppPlatformQoSEmergencyOnFlagReload3": "False",
      "DFFlagEnableAppPlatformQoSEmergencyOnFlagReload4": "False",
      "FFlagAndroidEnableQoS": "False",
      "FFlagEnableNetworkStatusObserving": "False",
      "DFFlagDontReportAccumulatedStatsInHttpClientDestroy2": "True",
      "FFlagEnableJNIAppbridgeStartMilestone": "False"
    }
  })json";
}

std::string ClientSettingsService::DefaultUrl(const std::string& application) {
  return "https://clientsettingscdn.roblox.com/v2/settings/application/" +
         application;
}

bool ClientSettingsService::IsValidJson(const std::string& content) {
  if (content.empty()) {
    return false;
  }
  const nlohmann::json value =
      nlohmann::json::parse(content, nullptr, false, true);
  return !value.is_discarded() && value.is_object();
}

const char* ClientSettingsSourceName(ClientSettingsSource source) {
  switch (source) {
    case ClientSettingsSource::kExplicitJson:
      return "explicit JSON";
    case ClientSettingsSource::kExplicitFile:
      return "explicit file";
    case ClientSettingsSource::kBundledFile:
      return "bundled file";
    case ClientSettingsSource::kSafeDefaults:
      return "safe defaults";
    case ClientSettingsSource::kDownloaded:
      return "download";
    case ClientSettingsSource::kCache:
      return "cache";
    case ClientSettingsSource::kEmptyDefaults:
      return "empty defaults";
  }
  return "unknown";
}

}  // namespace services
}  // namespace mocktail
