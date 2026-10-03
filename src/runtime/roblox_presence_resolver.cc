#include "runtime/roblox_presence_resolver.h"

#include <curl/curl.h>

#include <cstdlib>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

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

// Reads mocktail's own persisted session cookie from
// $XDG_DATA_HOME/mocktail/auth/roblox.cookie (default
// ~/.local/share/mocktail/auth/roblox.cookie), matching the format
// mocktail's own auth code already writes there: a single line of the
// form ".ROBLOSECURITY=<value>". Strips the prefix so callers get just
// the bare value. Returns an empty string if no session is stored yet.
std::string ReadStoredRoblosecurityCookie() {
  const char* home = std::getenv("HOME");
  if (home == nullptr || home[0] == '\0') {
    return std::string();
  }
  const char* xdg_data_home = std::getenv("XDG_DATA_HOME");
  const std::string base =
      (xdg_data_home != nullptr && xdg_data_home[0] != '\0')
          ? std::string(xdg_data_home)
          : std::string(home) + "/.local/share";
  const std::string path = base + "/mocktail/auth/roblox.cookie";

  std::ifstream file(path);
  if (!file.is_open()) {
    return std::string();
  }
  std::string line;
  std::getline(file, line);
  while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) {
    line.pop_back();
  }
  constexpr std::string_view kPrefix = ".ROBLOSECURITY=";
  if (line.size() > kPrefix.size() &&
      line.compare(0, kPrefix.size(), kPrefix) == 0) {
    return line.substr(kPrefix.size());
  }
  return line;
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
