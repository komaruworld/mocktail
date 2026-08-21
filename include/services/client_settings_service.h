#ifndef MOCKTAIL_SERVICES_CLIENT_SETTINGS_SERVICE_H_
#define MOCKTAIL_SERVICES_CLIENT_SETTINGS_SERVICE_H_

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>

#include "services/http_client.h"

namespace mocktail {
namespace services {

enum class ClientSettingsSource {
  kExplicitJson,
  kExplicitFile,
  kBundledFile,
  kSafeDefaults,
  kDownloaded,
  kCache,
  kEmptyDefaults,
};

struct ClientSettingsOptions {
  std::string explicit_json;
  std::filesystem::path explicit_file;
  std::filesystem::path bundled_file = "rbx_bin/client_settings_android.json";
  std::filesystem::path cache_file;
  std::string application = "GoogleAndroidApp";
  std::string url;
  bool use_bundled = false;
  bool sober_mode = true;
  bool fetch = false;
  bool auto_update = true;
};

struct ClientSettingsResult {
  std::string json;
  ClientSettingsSource source = ClientSettingsSource::kEmptyDefaults;
  bool valid_json = false;
  bool cache_updated = false;
  std::string error;
};

struct FflagsMergeResult {
  std::string json;
  bool loaded = false;
  std::size_t count = 0;
  std::string error;
};

// Loads a flat, optional fflags JSON file underneath caller-provided values.
// count is the number of file entries added after caller values take
// precedence.
FflagsMergeResult LoadAndMergeFflagsFile(const std::filesystem::path& path,
                                         std::string_view base_json);

class ClientSettingsService {
 public:
  explicit ClientSettingsService(HttpClient& http_client)
      : http_client_(http_client) {}

  ClientSettingsResult Resolve(const ClientSettingsOptions& options);

  static std::string SafeDefaultsJson();
  static std::string DefaultUrl(const std::string& application);
  static bool IsValidJson(const std::string& content);

 private:
  HttpClient& http_client_;
};

const char* ClientSettingsSourceName(ClientSettingsSource source);

}  // namespace services
}  // namespace mocktail

#endif  // MOCKTAIL_SERVICES_CLIENT_SETTINGS_SERVICE_H_
