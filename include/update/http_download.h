#ifndef MOCKTAIL_UPDATE_HTTP_DOWNLOAD_H_
#define MOCKTAIL_UPDATE_HTTP_DOWNLOAD_H_

#include <cstddef>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace mocktail::update {

struct HttpTransferRequest {
  std::string url;
  std::vector<std::string> headers;
  std::vector<std::string> allowed_hosts;
  std::size_t maximum_bytes = 0;
  long connect_timeout_ms = 10000;
  // Hard ceiling for one attempt; zero means none. A several hundred MiB
  // archive over a slow link is not a failed transfer, so the ceiling is the
  // wrong tool for it - a transfer that actually stops is caught by the
  // low-speed guard below, in a minute rather than in a quarter of an hour.
  long transfer_timeout_ms = 120000;
  long low_speed_bytes_per_second = 1024;
  long low_speed_seconds = 60;
  // Transient failures - a provider 503, a dropped connection, a stall - are
  // retried. A file transfer resumes from the bytes already on disk instead
  // of pulling the whole archive again.
  int maximum_attempts = 3;
  long retry_delay_ms = 2000;
  int maximum_redirects = 5;
};

struct HttpBytesResult {
  std::string bytes;
  long status_code = 0;
  std::string final_url;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

struct HttpDownloadResult {
  std::size_t bytes_written = 0;
  std::string sha256;
  std::string final_url;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

HttpBytesResult DownloadBytes(const HttpTransferRequest& request);

HttpDownloadResult DownloadFile(const HttpTransferRequest& request,
                                const std::filesystem::path& destination,
                                int progress_fd = -1);

bool IsTrustedHttpsUrl(std::string_view url,
                       const std::vector<std::string>& allowed_hosts,
                       std::string* error = nullptr);

}  // namespace mocktail::update

#endif  // MOCKTAIL_UPDATE_HTTP_DOWNLOAD_H_
