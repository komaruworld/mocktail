#include "update/http_download.h"

#include <curl/curl.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <ctime>
#include <limits>
#include <memory>
#include <mutex>
#include <string>

#include "update/payload_integrity.h"

namespace mocktail::update {
namespace {

std::once_flag g_curl_once;
CURLcode g_curl_status = CURLE_FAILED_INIT;

void InitialiseCurl() { g_curl_status = curl_global_init(CURL_GLOBAL_DEFAULT); }

bool HostMatches(std::string_view host, std::string_view allowed) {
  if (host == allowed) return true;
  return host.size() > allowed.size() &&
         host[host.size() - allowed.size() - 1] == '.' &&
         host.substr(host.size() - allowed.size()) == allowed;
}

std::string Lower(std::string value) {
  for (char& character : value) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character - 'A' + 'a');
    }
  }
  return value;
}

struct CurlUrlDeleter {
  void operator()(CURLU* url) const {
    if (url != nullptr) curl_url_cleanup(url);
  }
};

struct CurlStringDeleter {
  void operator()(char* value) const {
    if (value != nullptr) curl_free(value);
  }
};

struct CurlHandleDeleter {
  void operator()(CURL* handle) const {
    if (handle != nullptr) curl_easy_cleanup(handle);
  }
};

struct CurlHeadersDeleter {
  void operator()(curl_slist* headers) const {
    if (headers != nullptr) curl_slist_free_all(headers);
  }
};

bool RedirectStatus(long status) {
  return status == 301 || status == 302 || status == 303 || status == 307 ||
         status == 308;
}

bool RetryableTransport(CURLcode status) {
  switch (status) {
    case CURLE_COULDNT_CONNECT:
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_PARTIAL_FILE:
    case CURLE_RECV_ERROR:
    case CURLE_SEND_ERROR:
    case CURLE_GOT_NOTHING:
    case CURLE_SSL_CONNECT_ERROR:
      return true;
    default:
      return false;
  }
}

bool RetryableStatus(long status) {
  return status == 408 || status == 425 || status == 429 || status >= 500;
}

void Sleep(long milliseconds) {
  if (milliseconds <= 0) return;
  struct timespec delay = {milliseconds / 1000,
                           (milliseconds % 1000) * 1000000L};
  while (nanosleep(&delay, &delay) != 0 && errno == EINTR) {
  }
}

void PublishMessage(int descriptor, const std::string& message) {
  if (descriptor < 0) return;
  const std::string packet = "P" + message;
  (void)send(descriptor, packet.data(), packet.size(), MSG_NOSIGNAL);
}

constexpr curl_off_t kMebibyte = 1024 * 1024;

struct TransferProgress {
  int descriptor = -1;
  curl_off_t resumed_bytes = 0;
  std::chrono::steady_clock::time_point reported_at{};
  curl_off_t reported_bytes = 0;
  bool started = false;
};

// One decimal place without dragging in a stream: 1536 KiB/s reads as 1.5.
std::string Rate(curl_off_t bytes, std::chrono::milliseconds elapsed) {
  if (elapsed.count() <= 0) return {};
  const curl_off_t per_second = bytes * 1000 / elapsed.count();
  if (per_second < kMebibyte) {
    return " at " + std::to_string(per_second / 1024) + " KiB/s";
  }
  const curl_off_t tenths = per_second * 10 / kMebibyte;
  return " at " + std::to_string(tenths / 10) + "." +
         std::to_string(tenths % 10) + " MiB/s";
}

// Refreshed once a second: a dialog that only moves every 64 MiB looks frozen.
int ReportTransferProgress(void* opaque, curl_off_t expected, curl_off_t now,
                           curl_off_t, curl_off_t) {
  auto* progress = static_cast<TransferProgress*>(opaque);
  if (progress == nullptr || progress->descriptor < 0) return 0;
  const auto moment = std::chrono::steady_clock::now();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      moment - progress->reported_at);
  if (progress->started && elapsed < std::chrono::seconds(1)) return 0;
  const curl_off_t received = progress->resumed_bytes + now;
  const curl_off_t total =
      expected > 0 ? progress->resumed_bytes + expected : 0;
  std::string message =
      "Downloading Roblox... " + std::to_string(received / kMebibyte) + " MiB";
  if (total > 0) {
    message += " / " + std::to_string(total / kMebibyte) + " MiB (" +
               std::to_string(received * 100 / total) + "%)";
  }
  if (progress->started && received > progress->reported_bytes) {
    message += Rate(received - progress->reported_bytes, elapsed);
  }
  progress->started = true;
  progress->reported_at = moment;
  progress->reported_bytes = received;
  PublishMessage(progress->descriptor, message);
  return 0;
}

struct BytesWriter {
  std::string* output = nullptr;
  std::size_t maximum = 0;
  bool exceeded = false;
};

std::size_t WriteBytes(char* data, std::size_t size, std::size_t count,
                       void* opaque) {
  auto* writer = static_cast<BytesWriter*>(opaque);
  if (writer == nullptr || writer->output == nullptr || data == nullptr ||
      (count != 0 && size > std::numeric_limits<std::size_t>::max() / count)) {
    return 0;
  }
  const std::size_t bytes = size * count;
  if (bytes > writer->maximum ||
      writer->output->size() > writer->maximum - bytes) {
    writer->exceeded = true;
    return 0;
  }
  writer->output->append(data, bytes);
  return bytes;
}

struct FileWriter {
  int descriptor = -1;
  std::size_t maximum = 0;
  // Includes any resumed prefix, so the limit covers the whole file.
  std::size_t written = 0;
  // Null while resuming; the digest is then taken in one pass at the end.
  FileDigest* sha256 = nullptr;
  bool exceeded = false;
  bool write_failed = false;
  bool hash_failed = false;
};

std::size_t WriteFileBytes(char* data, std::size_t size, std::size_t count,
                           void* opaque) {
  auto* writer = static_cast<FileWriter*>(opaque);
  if (writer == nullptr || writer->descriptor < 0 || data == nullptr ||
      (count != 0 && size > std::numeric_limits<std::size_t>::max() / count)) {
    return 0;
  }
  const std::size_t bytes = size * count;
  if (bytes > writer->maximum || writer->written > writer->maximum - bytes) {
    writer->exceeded = true;
    return 0;
  }
  std::size_t offset = 0;
  while (offset < bytes) {
    const ssize_t result =
        write(writer->descriptor, data + offset, bytes - offset);
    if (result < 0) {
      if (errno == EINTR) continue;
      writer->write_failed = true;
      return 0;
    }
    offset += static_cast<std::size_t>(result);
  }
  if (writer->sha256 != nullptr) {
    if (!writer->sha256->Update(data, bytes)) {
      writer->hash_failed = true;
      return 0;
    }
  }
  writer->written += bytes;
  return bytes;
}

bool Configure(CURL* handle, const HttpTransferRequest& request,
               std::string_view url, curl_slist* headers,
               std::array<char, CURL_ERROR_SIZE>* error_buffer) {
  return curl_easy_setopt(handle, CURLOPT_URL, std::string(url).c_str()) ==
             CURLE_OK &&
         curl_easy_setopt(handle, CURLOPT_HTTPGET, 1L) == CURLE_OK &&
         curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 0L) == CURLE_OK &&
         curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "https") == CURLE_OK &&
         curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, "https") ==
             CURLE_OK &&
         curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L) == CURLE_OK &&
         curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS,
                          request.connect_timeout_ms) == CURLE_OK &&
         curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS,
                          request.transfer_timeout_ms) == CURLE_OK &&
         curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT,
                          request.low_speed_bytes_per_second) == CURLE_OK &&
         curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME,
                          request.low_speed_seconds) == CURLE_OK &&
         curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "") == CURLE_OK &&
         curl_easy_setopt(handle, CURLOPT_USERAGENT,
                          "Mocktail-native-updater/1") == CURLE_OK &&
         curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, error_buffer->data()) ==
             CURLE_OK &&
         (headers == nullptr ||
          curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers) == CURLE_OK);
}

std::unique_ptr<curl_slist, CurlHeadersDeleter> BuildHeaders(
    const std::vector<std::string>& values, std::string* error) {
  curl_slist* list = nullptr;
  for (const std::string& value : values) {
    curl_slist* next = curl_slist_append(list, value.c_str());
    if (next == nullptr) {
      curl_slist_free_all(list);
      *error = "cannot allocate HTTP request headers";
      return {};
    }
    list = next;
  }
  return std::unique_ptr<curl_slist, CurlHeadersDeleter>(list);
}

std::string RedirectUrl(CURL* handle) {
  char* value = nullptr;
  if (curl_easy_getinfo(handle, CURLINFO_REDIRECT_URL, &value) != CURLE_OK ||
      value == nullptr) {
    return {};
  }
  return value;
}

std::string CurlFailure(CURLcode status,
                        const std::array<char, CURL_ERROR_SIZE>& buffer) {
  return buffer[0] != '\0' ? std::string(buffer.data())
                           : std::string(curl_easy_strerror(status));
}

std::string UrlHost(std::string_view url) {
  std::unique_ptr<CURLU, CurlUrlDeleter> parsed(curl_url());
  char* raw_host = nullptr;
  if (!parsed ||
      curl_url_set(parsed.get(), CURLUPART_URL, std::string(url).c_str(), 0) !=
          CURLUE_OK ||
      curl_url_get(parsed.get(), CURLUPART_HOST, &raw_host, 0) != CURLUE_OK ||
      raw_host == nullptr) {
    return {};
  }
  std::unique_ptr<char, CurlStringDeleter> host(raw_host);
  return Lower(host.get());
}

// Provider outages are the most common first-run failure, so the host that
// rejected the transfer belongs in the message the user actually reads.
std::string StatusFailure(std::string_view action, std::string_view url,
                          long status_code) {
  const std::string host = UrlHost(url);
  return (host.empty() ? std::string("HTTPS ") + std::string(action)
                       : host + " " + std::string(action)) +
         " returned status " + std::to_string(status_code);
}

HttpBytesResult DownloadBytesAttempt(const HttpTransferRequest& request,
                                     bool* retryable) {
  HttpBytesResult result;
  std::string current = request.url;
  for (int redirect = 0; redirect <= request.maximum_redirects; ++redirect) {
    std::unique_ptr<CURL, CurlHandleDeleter> handle(curl_easy_init());
    if (!handle) {
      result.error = "curl_easy_init failed";
      return result;
    }
    auto headers = BuildHeaders(request.headers, &result.error);
    if (!request.headers.empty() && !headers) return result;
    std::array<char, CURL_ERROR_SIZE> error_buffer{};
    if (!Configure(handle.get(), request, current, headers.get(),
                   &error_buffer)) {
      result.error = "cannot configure HTTPS request";
      return result;
    }
    result.bytes.clear();
    BytesWriter writer{&result.bytes, request.maximum_bytes, false};
    curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, WriteBytes);
    curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &writer);
    const CURLcode status = curl_easy_perform(handle.get());
    if (status != CURLE_OK) {
      result.error = writer.exceeded ? "HTTPS response exceeds its size limit"
                                     : CurlFailure(status, error_buffer);
      *retryable = !writer.exceeded && RetryableTransport(status);
      return result;
    }
    curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE,
                      &result.status_code);
    if (RedirectStatus(result.status_code)) {
      const std::string next = RedirectUrl(handle.get());
      if (next.empty() || redirect == request.maximum_redirects ||
          !IsTrustedHttpsUrl(next, request.allowed_hosts, &result.error)) {
        if (result.error.empty()) result.error = "invalid HTTPS redirect";
        return result;
      }
      current = next;
      continue;
    }
    if (result.status_code < 200 || result.status_code >= 300) {
      result.error = StatusFailure("request", current, result.status_code);
      *retryable = RetryableStatus(result.status_code);
      return result;
    }
    result.final_url = current;
    return result;
  }
  result.error = "too many HTTPS redirects";
  return result;
}

// Leaves the transferred bytes in `temporary`: they survive a retryable
// failure so the next attempt resumes instead of pulling the archive again.
HttpDownloadResult DownloadFileAttempt(const HttpTransferRequest& request,
                                       const std::filesystem::path& temporary,
                                       int progress_fd, bool* retryable) {
  HttpDownloadResult result;
  std::error_code filesystem_error;
  std::uintmax_t existing = std::filesystem::file_size(temporary,
                                                       filesystem_error);
  if (filesystem_error || existing > request.maximum_bytes) {
    std::filesystem::remove(temporary, filesystem_error);
    existing = 0;
  }
  // Outlives the redirect hops, or the count restarts at zero on each one.
  TransferProgress progress;
  progress.descriptor = progress_fd;
  progress.resumed_bytes = static_cast<curl_off_t>(existing);
  std::string current = request.url;
  for (int redirect = 0; redirect <= request.maximum_redirects; ++redirect) {
    const bool resuming = existing > 0;
    const int descriptor =
        open(temporary.c_str(),
             O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW |
                 (resuming ? 0 : O_TRUNC),
             0600);
    if (descriptor < 0) {
      result.error = "cannot create temporary download: " +
                     std::string(std::strerror(errno));
      return result;
    }
    if (resuming && lseek(descriptor, 0, SEEK_END) < 0) {
      close(descriptor);
      std::filesystem::remove(temporary, filesystem_error);
      result.error = "cannot resume temporary download: " +
                     std::string(std::strerror(errno));
      return result;
    }
    std::unique_ptr<CURL, CurlHandleDeleter> handle(curl_easy_init());
    auto headers = BuildHeaders(request.headers, &result.error);
    if (!handle || (!request.headers.empty() && !headers)) {
      close(descriptor);
      std::filesystem::remove(temporary, filesystem_error);
      if (result.error.empty()) result.error = "curl_easy_init failed";
      return result;
    }
    std::array<char, CURL_ERROR_SIZE> error_buffer{};
    if (!Configure(handle.get(), request, current, headers.get(),
                   &error_buffer)) {
      close(descriptor);
      std::filesystem::remove(temporary, filesystem_error);
      result.error = "cannot configure HTTPS download";
      return result;
    }
    FileDigest sha256;
    if (!resuming && !sha256.valid()) {
      close(descriptor);
      std::filesystem::remove(temporary, filesystem_error);
      result.error = "cannot initialize sha256 digest";
      return result;
    }
    FileWriter writer{descriptor, request.maximum_bytes,
                      static_cast<std::size_t>(existing),
                      resuming ? nullptr : &sha256, false, false};
    curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, WriteFileBytes);
    curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &writer);
    curl_easy_setopt(handle.get(), CURLOPT_NOPROGRESS,
                     progress_fd < 0 ? 1L : 0L);
    curl_easy_setopt(handle.get(), CURLOPT_XFERINFOFUNCTION,
                     ReportTransferProgress);
    curl_easy_setopt(handle.get(), CURLOPT_XFERINFODATA, &progress);
    if (resuming) {
      curl_easy_setopt(handle.get(), CURLOPT_RESUME_FROM_LARGE,
                       static_cast<curl_off_t>(existing));
    }
    const CURLcode status = curl_easy_perform(handle.get());
    long http_status = 0;
    curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &http_status);
    const bool synced = fsync(descriptor) == 0;
    close(descriptor);
    if (status != CURLE_OK || writer.write_failed || writer.hash_failed || !synced) {
      if (writer.hash_failed) {
        result.error = "cannot compute SHA-256 digest during download";
      } else if (writer.exceeded) {
        result.error = "HTTPS download exceeds its size limit";
      } else if (writer.write_failed || !synced) {
        result.error = "cannot persist HTTPS download";
      } else {
        result.error = CurlFailure(status, error_buffer);
        *retryable = RetryableTransport(status);
      }
      if (!*retryable) std::filesystem::remove(temporary, filesystem_error);
      return result;
    }
    if (RedirectStatus(http_status)) {
      // The redirect body was appended to the file; drop it, keep the prefix.
      if (truncate(temporary.c_str(), static_cast<off_t>(existing)) != 0) {
        std::filesystem::remove(temporary, filesystem_error);
        existing = 0;
      }
      const std::string next = RedirectUrl(handle.get());
      if (next.empty() || redirect == request.maximum_redirects ||
          !IsTrustedHttpsUrl(next, request.allowed_hosts, &result.error)) {
        std::filesystem::remove(temporary, filesystem_error);
        if (result.error.empty()) result.error = "invalid HTTPS redirect";
        return result;
      }
      current = next;
      continue;
    }
    // The range was ignored or the offset refused; either way the partial
    // file is unusable now.
    if (resuming && (http_status == 200 || http_status == 416)) {
      std::filesystem::remove(temporary, filesystem_error);
      result.error = http_status == 416
                         ? "HTTPS server rejected the resume offset"
                         : "HTTPS server ignored the resume request";
      *retryable = true;
      return result;
    }
    if (http_status < 200 || http_status >= 300 || writer.written == 0) {
      result.error = writer.written == 0
                         ? "HTTPS download is empty"
                         : StatusFailure("download", current, http_status);
      *retryable = RetryableStatus(http_status);
      if (!*retryable) std::filesystem::remove(temporary, filesystem_error);
      return result;
    }
    result.bytes_written = writer.written;
    result.sha256 = resuming ? HashRegularFile(temporary, &result.error)
                             : sha256.FinalHex();
    if (result.sha256.empty()) {
      std::filesystem::remove(temporary, filesystem_error);
      if (result.error.empty()) {
        result.error = "cannot finalize SHA-256 digest of downloaded file";
      }
      return result;
    }
    result.final_url = current;
    return result;
  }
  std::filesystem::remove(temporary, filesystem_error);
  result.error = "too many HTTPS redirects";
  return result;
}

}  // namespace

bool IsTrustedHttpsUrl(std::string_view url,
                       const std::vector<std::string>& allowed_hosts,
                       std::string* error) {
  std::unique_ptr<CURLU, CurlUrlDeleter> parsed(curl_url());
  if (!parsed || curl_url_set(parsed.get(), CURLUPART_URL,
                              std::string(url).c_str(), 0) != CURLUE_OK) {
    if (error != nullptr) *error = "URL is malformed";
    return false;
  }
  char* raw_scheme = nullptr;
  char* raw_host = nullptr;
  char* raw_port = nullptr;
  char* raw_user = nullptr;
  const CURLUcode scheme_status =
      curl_url_get(parsed.get(), CURLUPART_SCHEME, &raw_scheme, 0);
  const CURLUcode host_status =
      curl_url_get(parsed.get(), CURLUPART_HOST, &raw_host, 0);
  const CURLUcode port_status =
      curl_url_get(parsed.get(), CURLUPART_PORT, &raw_port, 0);
  const CURLUcode user_status =
      curl_url_get(parsed.get(), CURLUPART_USER, &raw_user, 0);
  std::unique_ptr<char, CurlStringDeleter> scheme(raw_scheme);
  std::unique_ptr<char, CurlStringDeleter> host(raw_host);
  std::unique_ptr<char, CurlStringDeleter> port(raw_port);
  std::unique_ptr<char, CurlStringDeleter> user(raw_user);
  if (scheme_status != CURLUE_OK || host_status != CURLUE_OK ||
      scheme == nullptr || host == nullptr || Lower(scheme.get()) != "https" ||
      (port_status == CURLUE_OK && port != nullptr &&
       std::string_view(port.get()) != "443") ||
      (user_status == CURLUE_OK && user != nullptr && user.get()[0] != '\0')) {
    if (error != nullptr)
      *error = "URL must be credential-free HTTPS on port 443";
    return false;
  }
  std::string normalized_host = Lower(host.get());
  while (!normalized_host.empty() && normalized_host.back() == '.') {
    normalized_host.pop_back();
  }
  for (const std::string& allowed : allowed_hosts) {
    if (HostMatches(normalized_host, Lower(allowed))) return true;
  }
  if (error != nullptr) *error = "URL host is not trusted";
  return false;
}

HttpBytesResult DownloadBytes(const HttpTransferRequest& request) {
  HttpBytesResult result;
  if (request.maximum_bytes == 0 ||
      !IsTrustedHttpsUrl(request.url, request.allowed_hosts, &result.error)) {
    if (result.error.empty()) result.error = "HTTP byte limit is zero";
    return result;
  }
  std::call_once(g_curl_once, InitialiseCurl);
  if (g_curl_status != CURLE_OK) {
    result.error = curl_easy_strerror(g_curl_status);
    return result;
  }
  const int attempts =
      request.maximum_attempts > 0 ? request.maximum_attempts : 1;
  for (int attempt = 1;; ++attempt) {
    bool retryable = false;
    result = DownloadBytesAttempt(request, &retryable);
    if (result || !retryable || attempt >= attempts) return result;
    Sleep(request.retry_delay_ms * attempt);
  }
}

HttpDownloadResult DownloadFile(const HttpTransferRequest& request,
                                const std::filesystem::path& destination,
                                int progress_fd) {
  HttpDownloadResult result;
  if (request.maximum_bytes == 0 ||
      !IsTrustedHttpsUrl(request.url, request.allowed_hosts, &result.error)) {
    if (result.error.empty()) result.error = "HTTP byte limit is zero";
    return result;
  }
  std::call_once(g_curl_once, InitialiseCurl);
  if (g_curl_status != CURLE_OK) {
    result.error = curl_easy_strerror(g_curl_status);
    return result;
  }
  std::error_code filesystem_error;
  std::filesystem::create_directories(destination.parent_path(),
                                      filesystem_error);
  if (filesystem_error) {
    result.error = "cannot create download directory";
    return result;
  }
  const std::filesystem::path temporary =
      destination.parent_path() / ("." + destination.filename().string() +
                                   ".part-" + std::to_string(getpid()));
  std::filesystem::remove(temporary, filesystem_error);
  const int attempts =
      request.maximum_attempts > 0 ? request.maximum_attempts : 1;
  for (int attempt = 1;; ++attempt) {
    bool retryable = false;
    result = DownloadFileAttempt(request, temporary, progress_fd, &retryable);
    if (result) break;
    if (!retryable || attempt >= attempts) {
      std::filesystem::remove(temporary, filesystem_error);
      return result;
    }
    PublishMessage(progress_fd, "Retrying Roblox download...");
    Sleep(request.retry_delay_ms * attempt);
  }
  std::filesystem::rename(temporary, destination, filesystem_error);
  if (filesystem_error) {
    std::filesystem::remove(temporary, filesystem_error);
    result.error = "cannot publish completed download";
  }
  return result;
}

}  // namespace mocktail::update
