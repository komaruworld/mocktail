#include "services/auth_service.h"

#define JSON_NOEXCEPTION 1
#include <cctype>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace mocktail {
namespace services {
namespace {

constexpr std::string_view kCookieName = ".ROBLOSECURITY";
constexpr std::string_view kAuthenticatedUserUrl =
    "https://users.roblox.com/v1/users/authenticated";

bool IsCookieDelimiter(char ch) {
  return ch == ';' || ch == '\r' || ch == '\n' || ch == '\t';
}

struct ExtractedCookie {
  bool present = false;
  std::string_view value;
};

std::string_view TrimAsciiWhitespace(std::string_view value) {
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.front()))) {
    value.remove_prefix(1);
  }
  while (!value.empty() &&
         std::isspace(static_cast<unsigned char>(value.back()))) {
    value.remove_suffix(1);
  }
  return value;
}

bool IsSafeCookieValue(std::string_view value) {
  if (value.empty()) {
    return false;
  }
  for (const unsigned char character : value) {
    // Roblox security cookies are ASCII cookie-octets. Reject whitespace,
    // controls and non-ASCII bytes instead of placing them in an HTTP header.
    if (character <= 0x20 || character >= 0x7f || character == ';') {
      return false;
    }
  }
  return true;
}

ExtractedCookie ExtractCookie(std::string_view header) {
  if (header.rfind("_|", 0) == 0) {
    size_t end = 0;
    while (end < header.size() && !IsCookieDelimiter(header[end])) {
      ++end;
    }
    return {true, TrimAsciiWhitespace(header.substr(0, end))};
  }

  size_t segment_begin = 0;
  while (segment_begin < header.size()) {
    while (segment_begin < header.size() &&
           (IsCookieDelimiter(header[segment_begin]) ||
            std::isspace(static_cast<unsigned char>(header[segment_begin])))) {
      ++segment_begin;
    }
    size_t segment_end = segment_begin;
    while (segment_end < header.size() &&
           !IsCookieDelimiter(header[segment_end])) {
      ++segment_end;
    }

    const std::string_view segment = TrimAsciiWhitespace(
        header.substr(segment_begin, segment_end - segment_begin));
    const size_t equals = segment.find('=');
    const std::string_view name = TrimAsciiWhitespace(segment.substr(
        0, equals == std::string_view::npos ? segment.size() : equals));
    if (name == kCookieName) {
      if (equals == std::string_view::npos) {
        return {true, {}};
      }
      return {true, TrimAsciiWhitespace(segment.substr(equals + 1))};
    }
    segment_begin = segment_end + (segment_end < header.size() ? 1 : 0);
  }
  return {};
}

bool IsRoblosecuritySegment(std::string_view segment) {
  segment = TrimAsciiWhitespace(segment);
  const size_t equals = segment.find('=');
  const std::string_view name = TrimAsciiWhitespace(segment.substr(
      0, equals == std::string_view::npos ? segment.size() : equals));
  return name == kCookieName;
}

bool ParsePositiveUserId(const nlohmann::json& value, int64_t* user_id) {
  if (value.is_number_unsigned()) {
    const uint64_t unsigned_id = value.get<uint64_t>();
    if (unsigned_id == 0 ||
        unsigned_id >
            static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
      return false;
    }
    *user_id = static_cast<int64_t>(unsigned_id);
    return true;
  }
  if (!value.is_number_integer()) {
    return false;
  }
  const int64_t signed_id = value.get<int64_t>();
  if (signed_id <= 0) {
    return false;
  }
  *user_id = signed_id;
  return true;
}

bool ParseIdentity(const std::string& body, AuthIdentity* identity) {
  const nlohmann::json document = nlohmann::json::parse(body, nullptr, false);
  if (document.is_discarded() || !document.is_object()) {
    return false;
  }

  const auto id = document.find("id");
  const auto name = document.find("name");
  const auto display_name = document.find("displayName");
  if (id == document.end() || name == document.end() ||
      display_name == document.end() || !name->is_string() ||
      !display_name->is_string()) {
    return false;
  }

  int64_t user_id = -1;
  if (!ParsePositiveUserId(*id, &user_id)) {
    return false;
  }
  const std::string& username = name->get_ref<const std::string&>();
  if (username.empty()) {
    return false;
  }

  identity->user_id = user_id;
  identity->username = username;
  identity->display_name = display_name->get_ref<const std::string&>();
  return true;
}

}  // namespace

bool AuthService::HasRoblosecurityCookie(const std::string& cookie_header) {
  return ExtractCookie(cookie_header).present;
}

std::string AuthService::ExtractRoblosecurityValue(
    std::string_view cookie_header) {
  return std::string(ExtractCookie(cookie_header).value);
}

bool AuthService::RedactRejectedRoblosecurity(std::string_view cookie_header,
                                              std::string_view rejected_value,
                                              std::string* redacted_header) {
  if (redacted_header == nullptr) {
    return false;
  }
  redacted_header->clear();
  if (rejected_value.empty()) {
    return false;
  }
  const ExtractedCookie effective = ExtractCookie(cookie_header);
  if (!effective.present || effective.value != rejected_value) {
    return false;
  }

  redacted_header->assign(cookie_header);
  bool redacted = false;
  size_t segment_begin = 0;

  // A raw security value is accepted as the first file segment for backwards
  // compatibility. Redact it before processing any named segments that may
  // follow on later delimiter-separated lines.
  if (cookie_header.rfind("_|", 0) == 0) {
    size_t segment_end = 0;
    while (segment_end < cookie_header.size() &&
           !IsCookieDelimiter(cookie_header[segment_end])) {
      ++segment_end;
    }
    std::fill(redacted_header->begin(), redacted_header->begin() + segment_end,
              ' ');
    segment_begin = segment_end;
    if (segment_begin < cookie_header.size()) {
      ++segment_begin;
    }
    redacted = true;
  }

  while (segment_begin < cookie_header.size()) {
    size_t segment_end = segment_begin;
    while (segment_end < cookie_header.size() &&
           !IsCookieDelimiter(cookie_header[segment_end])) {
      ++segment_end;
    }
    const size_t next_segment =
        segment_end + (segment_end < cookie_header.size() ? 1 : 0);
    const std::string_view segment =
        cookie_header.substr(segment_begin, segment_end - segment_begin);
    if (IsRoblosecuritySegment(segment)) {
      std::fill(redacted_header->begin() + segment_begin,
                redacted_header->begin() + segment_end, ' ');
      redacted = true;
    }
    segment_begin = next_segment;
  }
  return redacted;
}

AuthSession AuthService::ResolveSession(std::string_view cookie_header,
                                        bool allow_guest) {
  AuthSession session;
  const ExtractedCookie cookie = ExtractCookie(cookie_header);
  if (!cookie.present) {
    if (allow_guest) {
      session.status = AuthSessionStatus::kGuest;
      return session;
    }
    session.status = AuthSessionStatus::kInvalid;
    session.error = "missing .ROBLOSECURITY cookie";
    return session;
  }
  if (!IsSafeCookieValue(cookie.value)) {
    session.status = AuthSessionStatus::kInvalid;
    session.error = "invalid .ROBLOSECURITY cookie";
    return session;
  }

  HttpRequest request;
  request.url = std::string(kAuthenticatedUserUrl);
  request.timeout_ms = 15000;
  request.maximum_body_bytes = 1024 * 1024;
  // Never forward a raw security cookie to another origin through an
  // unexpected redirect.
  request.follow_redirects = false;
  request.headers.push_back("Accept: application/json");
  request.headers.push_back("Cookie: .ROBLOSECURITY=" +
                            std::string(cookie.value));
  HttpResponse response = http_client_.Get(request);
  session.http_status = response.status_code;
  if (!response.transport_ok) {
    session.status = AuthSessionStatus::kUnavailable;
    // Do not propagate backend error strings: a custom HttpClient may include
    // request headers in them, which would disclose the security cookie.
    session.error = "authentication service unavailable";
  } else if (response.status_code == 200) {
    if (!ParseIdentity(response.body, &session.identity)) {
      session.status = AuthSessionStatus::kUnavailable;
      session.error = "invalid authenticated-user response";
      return session;
    }
    session.status = AuthSessionStatus::kAuthenticated;
  } else if (response.status_code == 401 || response.status_code == 403) {
    session.status = AuthSessionStatus::kInvalid;
    session.error = "authentication rejected";
  } else {
    session.status = AuthSessionStatus::kUnavailable;
    session.error = "unexpected authentication HTTP status";
  }
  return session;
}

AuthCookieValidation AuthService::ValidateCookie(
    const std::string& cookie_header) {
  const AuthSession session = ResolveSession(cookie_header, false);
  AuthCookieValidation validation;
  validation.http_status = session.http_status;
  validation.error = session.error;
  switch (session.status) {
    case AuthSessionStatus::kAuthenticated:
      validation.status = AuthCookieStatus::kValid;
      break;
    case AuthSessionStatus::kGuest:
    case AuthSessionStatus::kInvalid:
      validation.status = AuthCookieStatus::kInvalid;
      break;
    case AuthSessionStatus::kUnavailable:
      validation.status = AuthCookieStatus::kUnavailable;
      break;
  }
  return validation;
}

}  // namespace services
}  // namespace mocktail
