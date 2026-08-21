#include "update/http_download.h"

#include <curl/curl.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <string>

#include "mocktail/sha256.h"

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

void PublishProgress(int descriptor, std::size_t bytes) {
  if (descriptor < 0) return;
  const std::string packet = "PDownloading Roblox... " +
                             std::to_string(bytes / (1024U * 1024U)) + " MiB";
  (void)send(descriptor, packet.data(), packet.size(), MSG_NOSIGNAL);
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
  std::size_t written = 0;
  std::size_t next_progress = 64U * 1024U * 1024U;
  int progress_fd = -1;
  foundation::Sha256* sha256 = nullptr;
  bool exceeded = false;
  bool write_failed = false;
};

std::size_t WriteFileBytes(char* data, std::size_t size, std::size_t count,
                           void* opaque) {
  auto* writer = static_cast<FileWriter*>(opaque);
  if (writer == nullptr || writer->descriptor < 0 || data == nullptr ||
      writer->sha256 == nullptr ||
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
  writer->sha256->Update(reinterpret_cast<unsigned char*>(data), bytes);
  writer->written += bytes;
  if (writer->written >= writer->next_progress) {
    PublishProgress(writer->progress_fd, writer->written);
    writer->next_progress += 64U * 1024U * 1024U;
  }
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
      result.error =
          "HTTPS request returned status " + std::to_string(result.status_code);
      return result;
    }
    result.final_url = current;
    return result;
  }
  result.error = "too many HTTPS redirects";
  return result;
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
  std::string current = request.url;
  for (int redirect = 0; redirect <= request.maximum_redirects; ++redirect) {
    const int descriptor =
        open(temporary.c_str(),
             O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor < 0) {
      result.error = "cannot create temporary download: " +
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
    foundation::Sha256 sha256;
    FileWriter writer{descriptor,  request.maximum_bytes,
                      0,           64U * 1024U * 1024U,
                      progress_fd, &sha256,
                      false,       false};
    curl_easy_setopt(handle.get(), CURLOPT_WRITEFUNCTION, WriteFileBytes);
    curl_easy_setopt(handle.get(), CURLOPT_WRITEDATA, &writer);
    const CURLcode status = curl_easy_perform(handle.get());
    long http_status = 0;
    curl_easy_getinfo(handle.get(), CURLINFO_RESPONSE_CODE, &http_status);
    const bool synced = fsync(descriptor) == 0;
    close(descriptor);
    if (status != CURLE_OK || writer.write_failed || !synced) {
      std::filesystem::remove(temporary, filesystem_error);
      if (writer.exceeded) {
        result.error = "HTTPS download exceeds its size limit";
      } else if (writer.write_failed || !synced) {
        result.error = "cannot persist HTTPS download";
      } else {
        result.error = CurlFailure(status, error_buffer);
      }
      return result;
    }
    if (RedirectStatus(http_status)) {
      const std::string next = RedirectUrl(handle.get());
      std::filesystem::remove(temporary, filesystem_error);
      if (next.empty() || redirect == request.maximum_redirects ||
          !IsTrustedHttpsUrl(next, request.allowed_hosts, &result.error)) {
        if (result.error.empty()) result.error = "invalid HTTPS redirect";
        return result;
      }
      current = next;
      continue;
    }
    if (http_status < 200 || http_status >= 300 || writer.written == 0) {
      std::filesystem::remove(temporary, filesystem_error);
      result.error = writer.written == 0 ? "HTTPS download is empty"
                                         : "HTTPS download returned status " +
                                               std::to_string(http_status);
      return result;
    }
    std::filesystem::rename(temporary, destination, filesystem_error);
    if (filesystem_error) {
      std::filesystem::remove(temporary, filesystem_error);
      result.error = "cannot publish completed download";
      return result;
    }
    result.bytes_written = writer.written;
    result.sha256 = sha256.FinalHex();
    result.final_url = current;
    return result;
  }
  result.error = "too many HTTPS redirects";
  return result;
}

}  // namespace mocktail::update
