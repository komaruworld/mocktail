#ifndef MOCKTAIL_RUNTIME_ROBLOX_PRESENCE_RESOLVER_H_
#define MOCKTAIL_RUNTIME_ROBLOX_PRESENCE_RESOLVER_H_

#include <cstdint>
#include <memory>
#include <string>

#include "mocktail/status.h"

namespace mocktail {
namespace runtime {

class RobloxSecretStore;

struct ResolvedFollowUserPlace {
  int64_t place_id = 0;
  std::string game_instance_id;  // Roblox's "gameId" for the running session.
};

std::string ReadStoredRoblosecurityCookie(
    std::shared_ptr<RobloxSecretStore> secrets = nullptr);

// Calls Roblox's presence API to find what place/instance a user is
// currently in. `auth_cookie` is the runtime's already-authenticated
// .ROBLOSECURITY (or equivalent bearer token) — this function performs no
// authentication of its own and requires the runtime's validated session.
//
// Returns an error if the user is offline, has presence hidden, or is not
// in a joinable experience (e.g. in Studio, or privacy settings block it).
Status ResolveFollowUserPlace(int64_t user_id, const std::string& auth_cookie,
                              ResolvedFollowUserPlace* out);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_ROBLOX_PRESENCE_RESOLVER_H_
