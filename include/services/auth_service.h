#ifndef MOCKTAIL_SERVICES_AUTH_SERVICE_H_
#define MOCKTAIL_SERVICES_AUTH_SERVICE_H_

#include <cstdint>
#include <string>
#include <string_view>

#include "services/http_client.h"

namespace mocktail {
namespace services {

enum class AuthCookieStatus {
  kValid,
  kInvalid,
  kUnavailable,
};

struct AuthCookieValidation {
  AuthCookieStatus status = AuthCookieStatus::kUnavailable;
  long http_status = 0;
  std::string error;
};

enum class AuthSessionStatus {
  kAuthenticated,
  kGuest,
  kInvalid,
  kUnavailable,
};

struct AuthIdentity {
  int64_t user_id = -1;
  std::string username;
  std::string display_name;
};

// The security cookie is not retained in this result. Callers
// that need to inject it into the native runtime must keep their credentials
// separately and must not log them with the resolved identity.
struct AuthSession {
  AuthSessionStatus status = AuthSessionStatus::kUnavailable;
  AuthIdentity identity;
  long http_status = 0;
  std::string error;
};

class AuthService {
 public:
  explicit AuthService(HttpClient& http_client) : http_client_(http_client) {}

  // Resolves the current Roblox account from .ROBLOSECURITY. A missing cookie
  // becomes a guest session only when allow_guest is explicitly true.
  AuthSession ResolveSession(std::string_view cookie_header,
                             bool allow_guest = false);

  // Compatibility API for existing callers that only need cookie validity.
  AuthCookieValidation ValidateCookie(const std::string& cookie_header);
  static bool HasRoblosecurityCookie(const std::string& cookie_header);
  static std::string ExtractRoblosecurityValue(std::string_view cookie_header);

  // Redacts every exact .ROBLOSECURITY segment after confirming that the first
  // effective value is the rejected credential. Delimiters, file size, and
  // unrelated cookie bytes are preserved. Callers must treat redacted_header
  // as sensitive because it may still contain unrelated cookies.
  static bool RedactRejectedRoblosecurity(std::string_view cookie_header,
                                          std::string_view rejected_value,
                                          std::string* redacted_header);

 private:
  HttpClient& http_client_;
};

}  // namespace services
}  // namespace mocktail

#endif  // MOCKTAIL_SERVICES_AUTH_SERVICE_H_
