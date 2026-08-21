#include "services/http_client.h"

#include <curl/curl.h>

#include <array>
#include <cstdlib>
#include <limits>
#include <mutex>

#include "runtime/runtime_config.h"

namespace mocktail {
namespace services {
namespace {

std::once_flag g_curl_init_once;
CURLcode g_curl_init_result = CURLE_FAILED_INIT;

void InitialiseCurl() {
  g_curl_init_result = curl_global_init(CURL_GLOBAL_DEFAULT);
}

struct ResponseWriter {
  std::string* body = nullptr;
  size_t maximum_body_bytes = 0;
  bool limit_exceeded = false;
};

size_t WriteResponse(char* data, size_t size, size_t count, void* user_data) {
  auto* writer = static_cast<ResponseWriter*>(user_data);
  if (writer == nullptr || writer->body == nullptr || data == nullptr ||
      (count != 0 && size > std::numeric_limits<size_t>::max() / count)) {
    return 0;
  }
  const size_t bytes = size * count;
  if (bytes > writer->maximum_body_bytes ||
      writer->body->size() > writer->maximum_body_bytes - bytes) {
    writer->limit_exceeded = true;
    return 0;
  }
  writer->body->append(data, bytes);
  return bytes;
}

size_t WriteHeader(char* data, size_t size, size_t count, void* user_data) {
  auto* headers = static_cast<std::vector<std::string>*>(user_data);
  if (headers == nullptr || data == nullptr ||
      (count != 0 && size > std::numeric_limits<size_t>::max() / count)) {
    return 0;
  }
  const size_t bytes = size * count;
  std::string header(data, bytes);
  while (!header.empty() && (header.back() == '\r' || header.back() == '\n')) {
    header.pop_back();
  }
  if (!header.empty()) headers->push_back(std::move(header));
  return bytes;
}

}  // namespace

HttpResponse PerformRequest(const HttpRequest& request,
                            const std::string* post_body) {
  HttpResponse response;
  if (request.url.empty()) {
    response.error = "HTTP request URL is empty";
    return response;
  }

  std::call_once(g_curl_init_once, InitialiseCurl);
  if (g_curl_init_result != CURLE_OK) {
    response.error = curl_easy_strerror(g_curl_init_result);
    return response;
  }

  CURL* handle = curl_easy_init();
  if (handle == nullptr) {
    response.error = "curl_easy_init failed";
    return response;
  }

  curl_slist* headers = nullptr;
  for (const std::string& header : request.headers) {
    curl_slist* next = curl_slist_append(headers, header.c_str());
    if (next == nullptr) {
      curl_slist_free_all(headers);
      curl_easy_cleanup(handle);
      response.error = "could not allocate HTTP headers";
      return response;
    }
    headers = next;
  }

  std::array<char, CURL_ERROR_SIZE> error_buffer{};
  ResponseWriter writer{&response.body, request.maximum_body_bytes, false};
  curl_easy_setopt(handle, CURLOPT_URL, request.url.c_str());
  if (post_body == nullptr) {
    curl_easy_setopt(handle, CURLOPT_HTTPGET, 1L);
  } else {
    curl_easy_setopt(handle, CURLOPT_POST, 1L);
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, post_body->data());
    curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(post_body->size()));
  }
  curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION,
                   request.follow_redirects ? 1L : 0L);
  curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "https");
  curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, "https");
  curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(handle, CURLOPT_TIMEOUT_MS,
                   request.timeout_ms > 0 ? request.timeout_ms : 15000L);
  curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
  curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "");
  curl_easy_setopt(handle, CURLOPT_USERAGENT, "Mocktail/0.1");
  curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, error_buffer.data());
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, WriteResponse);
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, &writer);
  curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, WriteHeader);
  curl_easy_setopt(handle, CURLOPT_HEADERDATA, &response.headers);
  std::string proxy_url;
  const char* proxy_host = std::getenv("MOCKTAIL_HTTP_PROXY_HOST");
  const char* proxy_port = std::getenv("MOCKTAIL_HTTP_PROXY_PORT");
  const char* proxy_scheme = std::getenv("MOCKTAIL_HTTP_PROXY_SCHEME");
  if (proxy_host != nullptr && proxy_port != nullptr) {
    const std::optional<runtime::NetworkProxyConfig> proxy =
        runtime::ParseNetworkProxyConfig(
            proxy_host, proxy_port,
            proxy_scheme != nullptr ? proxy_scheme : "http");
    if (proxy.has_value()) {
      proxy_url = runtime::BuildNetworkProxyUrl(*proxy);
      curl_easy_setopt(handle, CURLOPT_PROXY, proxy_url.c_str());
    }
  }
  if (headers != nullptr) {
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
  }

  const CURLcode result = curl_easy_perform(handle);
  if (result == CURLE_OK) {
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &response.status_code);
    response.transport_ok = true;
  } else if (writer.limit_exceeded) {
    response.error = "HTTP response exceeded configured size limit";
  } else if (error_buffer[0] != '\0') {
    response.error = error_buffer.data();
  } else {
    response.error = curl_easy_strerror(result);
  }

  curl_slist_free_all(headers);
  curl_easy_cleanup(handle);
  return response;
}

HttpResponse CurlHttpClient::Get(const HttpRequest& request) {
  return PerformRequest(request, nullptr);
}

HttpResponse CurlHttpClient::Post(const HttpRequest& request,
                                  const std::string& body) {
  return PerformRequest(request, &body);
}

}  // namespace services
}  // namespace mocktail
