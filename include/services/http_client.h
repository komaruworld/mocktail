#ifndef MOCKTAIL_SERVICES_HTTP_CLIENT_H_
#define MOCKTAIL_SERVICES_HTTP_CLIENT_H_

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace mocktail {
namespace services {

struct HttpRequest {
  std::string url;
  std::vector<std::string> headers;
  long timeout_ms = 15000;
  size_t maximum_body_bytes = 16 * 1024 * 1024;
  bool follow_redirects = true;
};

struct HttpResponse {
  HttpResponse() = default;
  HttpResponse(bool transport, long status, std::string response_body,
               std::string response_error,
               std::vector<std::string> response_headers = {})
      : transport_ok(transport),
        status_code(status),
        body(std::move(response_body)),
        error(std::move(response_error)),
        headers(std::move(response_headers)) {}

  bool transport_ok = false;
  long status_code = 0;
  std::string body;
  std::string error;
  std::vector<std::string> headers;
};

class HttpClient {
 public:
  virtual ~HttpClient() = default;
  virtual HttpResponse Get(const HttpRequest& request) = 0;
  virtual HttpResponse Post(const HttpRequest& request,
                            const std::string& body) {
    HttpResponse response;
    response.error = "HTTP POST is not supported by this client";
    return response;
  }
};

// libcurl-backed production implementation. A new easy handle is used per
// request, so a single CurlHttpClient can be shared by runtime services.
class CurlHttpClient final : public HttpClient {
 public:
  HttpResponse Get(const HttpRequest& request) override;
  HttpResponse Post(const HttpRequest& request,
                    const std::string& body) override;
};

}  // namespace services
}  // namespace mocktail

#endif  // MOCKTAIL_SERVICES_HTTP_CLIENT_H_
