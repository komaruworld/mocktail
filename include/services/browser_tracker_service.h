#ifndef MOCKTAIL_SERVICES_BROWSER_TRACKER_SERVICE_H_
#define MOCKTAIL_SERVICES_BROWSER_TRACKER_SERVICE_H_

#include <filesystem>
#include <string>

#include "services/http_client.h"

namespace mocktail {
namespace services {

enum class BrowserTrackerStatus { kExisting, kCreated };

struct BrowserTrackerResult {
  BrowserTrackerStatus status = BrowserTrackerStatus::kExisting;
  std::string browser_tracker_id;
  long http_status = 0;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

class BrowserTrackerService {
 public:
  explicit BrowserTrackerService(HttpClient& http_client)
      : http_client_(http_client) {}

  BrowserTrackerResult EnsureInitialized(
      const std::filesystem::path& app_storage_file,
      const std::filesystem::path& cookie_file, bool allow_cookie_update);

 private:
  HttpClient& http_client_;
};

}  // namespace services
}  // namespace mocktail

#endif  // MOCKTAIL_SERVICES_BROWSER_TRACKER_SERVICE_H_
