#include "runtime/roblox_presence_resolver.h"

#include <curl/curl.h>

#include <cstdlib>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

#include "runtime/environment.h"
#include "runtime/roblox_cookie_store.h"
#include "runtime/runtime_paths.h"

namespace mocktail {
namespace runtime {
namespace {

Status Invalid(std::string message) {
  return Status::Error(StatusCode::kInvalidArgument, std::move(message));
}

size_t WriteToString(char* data, size_t size, size_t nmemb, void* userdata) {
  static_cast<std::string*>(userdata)->append(data, size * nmemb);
  return size * nmemb;
}

}  // namespace

std::string ReadStoredRoblosecurityCookie(
    std::shared_ptr<RobloxSecretStore> secrets) {
  const ProcessEnvironment environment;
  const RuntimePaths paths = RuntimePaths::FromEnvironment(environment);
  if (secrets == nullptr) secrets = OpenRobloxSecretStore();
  StoredCookie cookie;
  if (environment.HasNonEmpty("MOCKTAIL_ROBLOX_COOKIES")) {
    cookie.status = StoredCookieStatus::kFound;
    cookie.value = environment.GetOr("MOCKTAIL_ROBLOX_COOKIES", "");
  } else if (environment.HasNonEmpty("MOCKTAIL_COOKIE_FILE")) {
    cookie = ReadRobloxCookieFile(environment.GetOr("MOCKTAIL_COOKIE_FILE", ""),
                                  true);
  } else {
    cookie = LoadManagedRobloxCookie(paths.cookie_file(), *secrets);
  }
  if (cookie.status != StoredCookieStatus::kFound) return {};
  std::string value;
  std::string_view remaining(cookie.value);
  while (!remaining.empty()) {
    const auto end = remaining.find_first_of(";\r\n\t");
    auto segment = remaining.substr(0, end);
    while (!segment.empty() && segment.front() == ' ') segment.remove_prefix(1);
    constexpr std::string_view prefix = ".ROBLOSECURITY=";
    if (segment.substr(0, prefix.size()) == prefix) {
      value = segment.substr(prefix.size());
      break;
    }
    if (segment.substr(0, 2) == "_|") {
      value = segment;
      break;
    }
    if (end == remaining.npos) break;
    remaining.remove_prefix(end + 1);
  }
  volatile char* bytes = cookie.value.empty() ? nullptr : cookie.value.data();
  for (size_t i = 0; i < cookie.value.size(); ++i) bytes[i] = '\0';
  for (unsigned char ch : value) {
    if (ch <= 0x20 || ch >= 0x7f || ch == ';') return {};
  }
  return value;
}

Status ResolveFollowUserPlace(int64_t user_id, const std::string& auth_cookie,
                              ResolvedFollowUserPlace* out) {
  if (out == nullptr || user_id <= 0) {
    return Invalid("Roblox presence lookup requires a positive userId");
  }
  if (auth_cookie.empty()) {
    return Invalid("Roblox presence lookup requires an authenticated session");
  }

  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    return Invalid("Roblox presence lookup could not initialize a request");
  }

  const nlohmann::json body = {{"userIds", {user_id}}};
  const std::string payload = body.dump();
  std::string response;

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  const std::string cookie_header = "Cookie: .ROBLOSECURITY=" + auth_cookie;
  headers = curl_slist_append(headers, cookie_header.c_str());

  curl_easy_setopt(curl, CURLOPT_URL,
                   "https://presence.roblox.com/v1/presence/users");
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteToString);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);

  const CURLcode result = curl_easy_perform(curl);
  long http_status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_status);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (result != CURLE_OK) {
    return Invalid("Roblox presence lookup failed to connect");
  }
  if (http_status != 200) {
    return Invalid("Roblox presence lookup was rejected (status " +
                   std::to_string(http_status) + ")");
  }

  const nlohmann::json parsed = nlohmann::json::parse(
      response, /*cb=*/nullptr, /*allow_exceptions=*/false);
  if (parsed.is_discarded()) {
    return Invalid("Roblox presence lookup returned malformed JSON");
  }

  if (!parsed.contains("userPresences") ||
      !parsed["userPresences"].is_array() ||
      parsed["userPresences"].empty()) {
    return Invalid("Roblox presence lookup returned no data for user");
  }

  const auto& presence = parsed["userPresences"][0];
  // userPresenceType: 0 offline, 1 online, 2 in-game, 3 in-studio.
  const int presence_type = presence.value("userPresenceType", 0);
  if (presence_type != 2) {
    return Invalid("User is not currently in a joinable Roblox experience");
  }
  if (!presence.contains("placeId") || presence["placeId"].is_null()) {
    return Invalid("Roblox presence lookup did not include a placeId");
  }

  out->place_id = presence.value("placeId", int64_t{0});
  if (presence.contains("gameId") && !presence["gameId"].is_null()) {
    out->game_instance_id = presence.value("gameId", std::string());
  }
  if (out->place_id <= 0) {
    return Invalid("Roblox presence lookup returned an invalid placeId");
  }
  return Status::Ok();
}

}  // namespace runtime
}  // namespace mocktail
