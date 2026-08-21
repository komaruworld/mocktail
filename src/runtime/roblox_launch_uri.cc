#include "runtime/roblox_launch_uri.h"

#include <curl/curl.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <limits>
#include <nlohmann/json.hpp>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mocktail {
namespace runtime {
namespace {

constexpr std::size_t kMaximumParameterCount = 64;
constexpr std::size_t kMaximumParameterKeyBytes = 128;
constexpr std::size_t kMaximumParameterValueBytes = 16 * 1024;
constexpr std::string_view kModernExperiencePrefix = "//experiences/start?";
constexpr std::string_view kModernDirectPrefix = "//placeid=";
constexpr std::string_view kPlaceLauncherPath = "/game/placelauncher.ashx";

Status Invalid(std::string message) {
  return Status::Error(StatusCode::kInvalidArgument, std::move(message));
}

class ScopedCurlUrl final {
 public:
  ScopedCurlUrl() : handle_(curl_url()) {}
  ~ScopedCurlUrl() {
    if (handle_ != nullptr) {
      curl_url_cleanup(handle_);
    }
  }

  ScopedCurlUrl(const ScopedCurlUrl&) = delete;
  ScopedCurlUrl& operator=(const ScopedCurlUrl&) = delete;

  CURLU* get() const { return handle_; }

 private:
  CURLU* handle_ = nullptr;
};

class ScopedCurlString final {
 public:
  ScopedCurlString() = default;
  ~ScopedCurlString() { curl_free(value_); }

  ScopedCurlString(const ScopedCurlString&) = delete;
  ScopedCurlString& operator=(const ScopedCurlString&) = delete;

  char** output() { return &value_; }
  const char* get() const { return value_; }

 private:
  char* value_ = nullptr;
};

struct LaunchFields {
  int64_t place_id = 0;
  int64_t user_id = 0;
  int64_t conversation_id = 0;
  int64_t referred_by_player_id = 0;
  std::string request_type;
  std::string game_instance_id;
  std::string reserved_server_access_code;
  std::string call_id;
  std::string referral_page;
  std::string access_code;
  std::string link_code;
  std::string launch_data;
  std::string event_id;
  std::string game_join_context;
  std::string join_attempt_id;
  std::string join_attempt_origin;
  std::string iso_context;
  std::set<std::string> seen_fields;
};

std::string AsciiLower(std::string_view value) {
  std::string normalized(value);
  std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                 [](unsigned char character) {
                   return static_cast<char>(std::tolower(character));
                 });
  return normalized;
}

bool ContainsControlBytes(std::string_view value) {
  return std::any_of(value.begin(), value.end(), [](unsigned char character) {
    return character < 0x20 || character == 0x7f;
  });
}

bool IsValidUtf8(std::string_view value) {
  for (std::size_t index = 0; index < value.size();) {
    const unsigned char first = static_cast<unsigned char>(value[index]);
    if (first <= 0x7f) {
      ++index;
      continue;
    }
    std::size_t continuation_count = 0;
    uint32_t code_point = 0;
    if (first >= 0xc2 && first <= 0xdf) {
      continuation_count = 1;
      code_point = first & 0x1f;
    } else if (first >= 0xe0 && first <= 0xef) {
      continuation_count = 2;
      code_point = first & 0x0f;
    } else if (first >= 0xf0 && first <= 0xf4) {
      continuation_count = 3;
      code_point = first & 0x07;
    } else {
      return false;
    }
    if (index + continuation_count >= value.size()) {
      return false;
    }
    for (std::size_t offset = 1; offset <= continuation_count; ++offset) {
      const unsigned char continuation =
          static_cast<unsigned char>(value[index + offset]);
      if ((continuation & 0xc0) != 0x80) {
        return false;
      }
      code_point = (code_point << 6) | (continuation & 0x3f);
    }
    if ((continuation_count == 2 && code_point < 0x800) ||
        (continuation_count == 3 && code_point < 0x10000) ||
        (code_point >= 0xd800 && code_point <= 0xdfff) ||
        code_point > 0x10ffff) {
      return false;
    }
    index += continuation_count + 1;
  }
  return true;
}

int HexDigitValue(char character) {
  if (character >= '0' && character <= '9') {
    return character - '0';
  }
  if (character >= 'a' && character <= 'f') {
    return character - 'a' + 10;
  }
  if (character >= 'A' && character <= 'F') {
    return character - 'A' + 10;
  }
  return -1;
}

Status PercentDecode(std::string_view encoded, bool plus_as_space,
                     std::string* decoded) {
  if (decoded == nullptr || encoded.size() > kMaximumParameterValueBytes) {
    return Invalid("Roblox launch parameter size is invalid");
  }
  decoded->clear();
  decoded->reserve(encoded.size());
  for (std::size_t index = 0; index < encoded.size(); ++index) {
    unsigned char value = static_cast<unsigned char>(encoded[index]);
    if (encoded[index] == '%') {
      if (index + 2 >= encoded.size()) {
        return Invalid("Roblox launch URI contains invalid percent encoding");
      }
      const int high = HexDigitValue(encoded[index + 1]);
      const int low = HexDigitValue(encoded[index + 2]);
      if (high < 0 || low < 0) {
        return Invalid("Roblox launch URI contains invalid percent encoding");
      }
      value = static_cast<unsigned char>((high << 4) | low);
      index += 2;
    } else if (encoded[index] == '+' && plus_as_space) {
      value = ' ';
    }
    if (value < 0x20 || value == 0x7f) {
      return Invalid("Roblox launch URI contains a control byte");
    }
    decoded->push_back(static_cast<char>(value));
  }
  if (decoded->size() > kMaximumParameterValueBytes || !IsValidUtf8(*decoded)) {
    return Invalid("Roblox launch parameter is not bounded UTF-8");
  }
  return Status::Ok();
}

Status ReadDecimalInteger(std::string_view value, const char* field_name,
                          bool allow_zero, int64_t* output) {
  if (output == nullptr || value.empty() || value.front() == '+' ||
      value.front() == '-') {
    return Invalid(std::string("Roblox launch integer is invalid: ") +
                   field_name);
  }
  int64_t parsed = 0;
  const auto result =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc() || result.ptr != value.data() + value.size() ||
      parsed < 0 || (!allow_zero && parsed == 0)) {
    return Invalid(std::string("Roblox launch integer is invalid: ") +
                   field_name);
  }
  *output = parsed;
  return Status::Ok();
}

Status RememberField(std::string canonical_name, LaunchFields* fields) {
  if (!fields->seen_fields.insert(std::move(canonical_name)).second) {
    return Invalid("Roblox launch URI repeats a singleton field");
  }
  return Status::Ok();
}

Status AssignField(std::string_view raw_key, std::string value,
                   bool decode_modern_launch_data, LaunchFields* fields) {
  if (raw_key.empty() || raw_key.size() > kMaximumParameterKeyBytes ||
      fields == nullptr) {
    return Invalid("Roblox launch parameter key is invalid");
  }
  const std::string key = AsciiLower(raw_key);
  std::string canonical_key;
  if (key == "request") {
    canonical_key = "request";
  } else if (key == "placeid") {
    canonical_key = "placeId";
  } else if (key == "userid") {
    canonical_key = "userId";
  } else if (key == "conversationid") {
    canonical_key = "conversationId";
  } else if (key == "referredbyplayerid") {
    canonical_key = "referredByPlayerId";
  } else if (key == "gameid" || key == "gameinstanceid" ||
             key == "instanceid") {
    canonical_key = "gameInstanceId";
  } else if (key == "reservedserveraccesscode") {
    canonical_key = "reservedServerAccessCode";
  } else if (key == "callid") {
    canonical_key = "callId";
  } else if (key == "referralpage") {
    canonical_key = "referralPage";
  } else if (key == "accesscode") {
    canonical_key = "accessCode";
  } else if (key == "linkcode" || key == "privateserverlinkcode") {
    canonical_key = "linkCode";
  } else if (key == "launchdata") {
    canonical_key = "launchData";
  } else if (key == "eventid") {
    canonical_key = "eventId";
  } else if (key == "gamejoincontext") {
    canonical_key = "gameJoinContext";
  } else if (key == "joinattemptid") {
    canonical_key = "joinAttemptId";
  } else if (key == "joinattemptorigin") {
    canonical_key = "joinAttemptOrigin";
  } else if (key == "isocontext") {
    canonical_key = "isoContext";
  } else {
    return Status::Ok();
  }

  Status status = RememberField(canonical_key, fields);
  if (!status.ok()) {
    return status;
  }
  if (canonical_key == "launchData" && decode_modern_launch_data &&
      value.find('%') != std::string::npos) {
    std::string decoded;
    status = PercentDecode(value, false, &decoded);
    if (!status.ok()) {
      return status;
    }
    value = std::move(decoded);
  }
  if (canonical_key == "request") {
    fields->request_type = std::move(value);
  } else if (canonical_key == "placeId") {
    status = ReadDecimalInteger(value, "placeId", false, &fields->place_id);
  } else if (canonical_key == "userId") {
    status = ReadDecimalInteger(value, "userId", true, &fields->user_id);
  } else if (canonical_key == "conversationId") {
    status = ReadDecimalInteger(value, "conversationId", true,
                                &fields->conversation_id);
  } else if (canonical_key == "referredByPlayerId") {
    status = ReadDecimalInteger(value, "referredByPlayerId", true,
                                &fields->referred_by_player_id);
  } else if (canonical_key == "gameInstanceId") {
    fields->game_instance_id = std::move(value);
  } else if (canonical_key == "reservedServerAccessCode") {
    fields->reserved_server_access_code = std::move(value);
  } else if (canonical_key == "callId") {
    fields->call_id = std::move(value);
  } else if (canonical_key == "referralPage") {
    fields->referral_page = std::move(value);
  } else if (canonical_key == "accessCode") {
    fields->access_code = std::move(value);
  } else if (canonical_key == "linkCode") {
    fields->link_code = std::move(value);
  } else if (canonical_key == "launchData") {
    fields->launch_data = std::move(value);
  } else if (canonical_key == "eventId") {
    fields->event_id = std::move(value);
  } else if (canonical_key == "gameJoinContext") {
    fields->game_join_context = std::move(value);
  } else if (canonical_key == "joinAttemptId") {
    fields->join_attempt_id = std::move(value);
  } else if (canonical_key == "joinAttemptOrigin") {
    fields->join_attempt_origin = std::move(value);
  } else if (canonical_key == "isoContext") {
    fields->iso_context = std::move(value);
  }
  return status;
}

Status ParseQuery(std::string_view query, bool decode_modern_launch_data,
                  LaunchFields* fields) {
  if (query.empty() || fields == nullptr) {
    return Invalid("Roblox launch URI query is missing");
  }
  std::size_t count = 0;
  std::size_t begin = 0;
  while (begin <= query.size()) {
    if (++count > kMaximumParameterCount) {
      return Invalid("Roblox launch URI has too many parameters");
    }
    const std::size_t end = query.find('&', begin);
    const std::string_view pair =
        query.substr(begin, end == std::string_view::npos ? query.size() - begin
                                                          : end - begin);
    if (pair.empty()) {
      return Invalid("Roblox launch URI contains an empty parameter");
    }
    const std::size_t equals = pair.find('=');
    if (equals == std::string_view::npos || equals == 0) {
      return Invalid("Roblox launch URI parameter has no value");
    }
    std::string key;
    std::string value;
    Status status = PercentDecode(pair.substr(0, equals), true, &key);
    if (status.ok()) {
      status = PercentDecode(pair.substr(equals + 1), true, &value);
    }
    if (status.ok()) {
      status =
          AssignField(key, std::move(value), decode_modern_launch_data, fields);
    }
    if (!status.ok()) {
      return status;
    }
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1;
  }
  return Status::Ok();
}

Status GetRequiredUrlPart(CURLU* url, CURLUPart part, const char* label,
                          std::string* output) {
  ScopedCurlString value;
  if (url == nullptr || output == nullptr ||
      curl_url_get(url, part, value.output(), 0) != CURLUE_OK ||
      value.get() == nullptr) {
    return Invalid(std::string("Roblox PlaceLauncher URL has no ") + label);
  }
  *output = value.get();
  return Status::Ok();
}

bool HasUrlPart(CURLU* url, CURLUPart part) {
  ScopedCurlString value;
  return url != nullptr &&
         curl_url_get(url, part, value.output(), 0) == CURLUE_OK &&
         value.get() != nullptr && value.get()[0] != '\0';
}

Status ParsePlaceLauncherUrl(std::string_view launcher_url,
                             LaunchFields* fields) {
  if (launcher_url.empty() ||
      launcher_url.size() > kMaximumRobloxLaunchUriBytes ||
      ContainsControlBytes(launcher_url) || !IsValidUtf8(launcher_url)) {
    return Invalid("Roblox PlaceLauncher URL is invalid");
  }
  const ScopedCurlUrl parsed;
  const std::string owned_url(launcher_url);
  if (parsed.get() == nullptr ||
      curl_url_set(parsed.get(), CURLUPART_URL, owned_url.c_str(),
                   CURLU_NON_SUPPORT_SCHEME | CURLU_PATH_AS_IS) != CURLUE_OK) {
    return Invalid("Roblox PlaceLauncher URL cannot be parsed");
  }

  std::string scheme;
  std::string host;
  std::string path;
  std::string query;
  Status status =
      GetRequiredUrlPart(parsed.get(), CURLUPART_SCHEME, "scheme", &scheme);
  if (status.ok()) {
    status = GetRequiredUrlPart(parsed.get(), CURLUPART_HOST, "host", &host);
  }
  if (status.ok()) {
    status = GetRequiredUrlPart(parsed.get(), CURLUPART_PATH, "path", &path);
  }
  if (status.ok()) {
    status = GetRequiredUrlPart(parsed.get(), CURLUPART_QUERY, "query", &query);
  }
  if (!status.ok()) {
    return status;
  }
  const std::string normalized_host = AsciiLower(host);
  if (AsciiLower(scheme) != "https" ||
      (normalized_host != "www.roblox.com" && normalized_host != "roblox.com" &&
       normalized_host != "assetgame.roblox.com") ||
      AsciiLower(path) != kPlaceLauncherPath ||
      HasUrlPart(parsed.get(), CURLUPART_USER) ||
      HasUrlPart(parsed.get(), CURLUPART_PASSWORD) ||
      HasUrlPart(parsed.get(), CURLUPART_FRAGMENT)) {
    return Invalid("Roblox PlaceLauncher URL origin or path is not allowed");
  }
  ScopedCurlString port;
  const CURLUcode port_status =
      curl_url_get(parsed.get(), CURLUPART_PORT, port.output(), 0);
  if (port_status == CURLUE_OK && port.get() != nullptr &&
      std::string_view(port.get()) != "443") {
    return Invalid("Roblox PlaceLauncher URL port is not allowed");
  }
  return ParseQuery(query, false, fields);
}

Status ParseClassic(std::string_view payload, LaunchFields* fields) {
  if (payload.empty() || fields == nullptr) {
    return Invalid("Roblox player launch payload is missing");
  }
  std::size_t begin = 0;
  std::size_t count = 0;
  bool version_seen = false;
  bool launch_mode_seen = false;
  bool launcher_url_seen = false;
  std::string launcher_url;
  while (begin <= payload.size()) {
    if (++count > kMaximumParameterCount) {
      return Invalid("Roblox player launch payload has too many fields");
    }
    const std::size_t end = payload.find('+', begin);
    const std::string_view field = payload.substr(
        begin,
        end == std::string_view::npos ? payload.size() - begin : end - begin);
    if (field.size() >
        kMaximumParameterKeyBytes + 1 + kMaximumParameterValueBytes) {
      return Invalid("Roblox player launch field is too large");
    }
    if (!version_seen) {
      if (field != "1") {
        return Invalid("Roblox player launch version is unsupported");
      }
      version_seen = true;
    } else {
      const std::size_t colon = field.find(':');
      if (colon == std::string_view::npos || colon == 0) {
        return Invalid("Roblox player launch field is malformed");
      }
      const std::string key = AsciiLower(field.substr(0, colon));
      const std::string_view encoded_value = field.substr(colon + 1);
      if (key == "launchmode") {
        if (launch_mode_seen) {
          return Invalid("Roblox player launch repeats launchmode");
        }
        launch_mode_seen = true;
        std::string launch_mode;
        Status status = PercentDecode(encoded_value, false, &launch_mode);
        if (!status.ok()) {
          return status;
        }
        if (AsciiLower(launch_mode) != "play") {
          return Invalid("Roblox player launch mode is unsupported");
        }
      } else if (key == "placelauncherurl") {
        if (launcher_url_seen) {
          return Invalid("Roblox player launch repeats PlaceLauncher URL");
        }
        launcher_url_seen = true;
        Status status = PercentDecode(encoded_value, false, &launcher_url);
        if (!status.ok()) {
          return status;
        }
      }
      // gameinfo is an authentication ticket. Unknown outer metadata,
      // including gameinfo and browsertrackerid, is never decoded or retained.
    }
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1;
  }
  if (!version_seen || !launch_mode_seen || !launcher_url_seen) {
    return Invalid("Roblox player launch contract is incomplete");
  }
  Status status = ParsePlaceLauncherUrl(launcher_url, fields);
  if (status.ok() && fields->request_type.empty()) {
    status = Invalid("Roblox PlaceLauncher request type is missing");
  }
  return status;
}

Status ValidateSelector(const LaunchFields& fields) {
  const bool private_join =
      !fields.access_code.empty() || !fields.link_code.empty();
  const std::array<bool, 5> selectors = {
      fields.user_id > 0, fields.conversation_id > 0, private_join,
      !fields.game_instance_id.empty(),
      !fields.reserved_server_access_code.empty()};
  const std::size_t selector_count = static_cast<std::size_t>(
      std::count(selectors.begin(), selectors.end(), true));
  if (selector_count > 1) {
    return Invalid("Roblox launch URI contains conflicting join selectors");
  }
  if (fields.request_type.empty()) {
    return Status::Ok();
  }
  const std::string request = AsciiLower(fields.request_type);
  if (request == "requestgame") {
    return selector_count == 0
               ? Status::Ok()
               : Invalid("RequestGame contains a non-public join selector");
  }
  if (request == "requestgamejob") {
    return !fields.game_instance_id.empty()
               ? Status::Ok()
               : Invalid("RequestGameJob requires a game instance");
  }
  if (request == "requestprivategame") {
    return private_join || !fields.reserved_server_access_code.empty()
               ? Status::Ok()
               : Invalid("RequestPrivateGame requires an access code");
  }
  if (request == "requestplaytogethergame") {
    return fields.conversation_id > 0
               ? Status::Ok()
               : Invalid("RequestPlayTogetherGame requires a conversation");
  }
  return Invalid("Roblox PlaceLauncher request type is unsupported");
}

Status MakeRequest(const LaunchFields& fields,
                   RobloxExperienceLaunchRequest* request) {
  if (fields.place_id <= 0) {
    return Invalid("Roblox launch URI requires a positive placeId");
  }
  Status status = ValidateSelector(fields);
  if (!status.ok()) {
    return status;
  }
  nlohmann::json document = {{"placeId", fields.place_id}};
  const auto add_integer = [&document](const char* name, int64_t value) {
    if (value > 0) {
      document[name] = value;
    }
  };
  const auto add_string = [&document](const char* name,
                                      const std::string& value) {
    if (!value.empty()) {
      document[name] = value;
    }
  };
  add_integer("userId", fields.user_id);
  add_integer("conversationId", fields.conversation_id);
  add_integer("referredByPlayerId", fields.referred_by_player_id);
  add_string("gameInstanceId", fields.game_instance_id);
  add_string("reservedServerAccessCode", fields.reserved_server_access_code);
  add_string("callId", fields.call_id);
  add_string("referralPage", fields.referral_page);
  add_string("accessCode", fields.access_code);
  add_string("linkCode", fields.link_code);
  add_string("launchData", fields.launch_data);
  add_string("eventId", fields.event_id);
  add_string("gameJoinContext", fields.game_join_context);
  add_string("joinAttemptId", fields.join_attempt_id);
  add_string("joinAttemptOrigin", fields.join_attempt_origin);
  add_string("isoContext", fields.iso_context);
  return ParseRobloxExperienceLaunchJson(document.dump(), request);
}

Status ParseModern(std::string_view payload, LaunchFields* fields) {
  std::string_view query;
  bool decode_launch_data_twice = false;
  if (payload.size() > kModernExperiencePrefix.size() &&
      AsciiLower(payload.substr(0, kModernExperiencePrefix.size())) ==
          kModernExperiencePrefix) {
    query = payload.substr(kModernExperiencePrefix.size());
    decode_launch_data_twice = true;
  } else if (payload.size() >= kModernDirectPrefix.size() &&
             AsciiLower(payload.substr(0, kModernDirectPrefix.size())) ==
                 kModernDirectPrefix) {
    // Roblox's documented direct-app form is
    // roblox://placeId=<id>&..., without a '?' separator.
    query = payload.substr(2);
  } else {
    return Invalid("Roblox launch path is unsupported");
  }
  if (query.find('#') != std::string_view::npos ||
      query.find('/') != std::string_view::npos) {
    return Invalid("Roblox launch URI contains an unsupported fragment");
  }
  return ParseQuery(query, decode_launch_data_twice, fields);
}

}  // namespace

Status ParseRobloxLaunchUri(std::string_view uri,
                            RobloxExperienceLaunchRequest* request) {
  if (request == nullptr) {
    return Invalid("Roblox launch output is null");
  }
  *request = {};
  if (uri.empty() || uri.size() > kMaximumRobloxLaunchUriBytes ||
      ContainsControlBytes(uri) || !IsValidUtf8(uri)) {
    return Invalid("Roblox launch URI size or encoding is invalid");
  }
  const std::size_t colon = uri.find(':');
  if (colon == std::string_view::npos || colon == 0) {
    return Invalid("Roblox launch URI has no supported scheme");
  }
  const std::string scheme = AsciiLower(uri.substr(0, colon));
  const std::string_view payload = uri.substr(colon + 1);
  LaunchFields fields;
  Status status;
  if (scheme == "roblox-player") {
    status = ParseClassic(payload, &fields);
  } else if (scheme == "roblox") {
    status = ParseModern(payload, &fields);
  } else {
    return Invalid("Roblox launch URI scheme is unsupported");
  }
  if (!status.ok()) {
    return status;
  }
  return MakeRequest(fields, request);
}

}  // namespace runtime
}  // namespace mocktail
