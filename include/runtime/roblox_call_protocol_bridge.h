#ifndef MOCKTAIL_RUNTIME_ROBLOX_CALL_PROTOCOL_BRIDGE_H_
#define MOCKTAIL_RUNTIME_ROBLOX_CALL_PROTOCOL_BRIDGE_H_

#include "runtime/roblox_message_bus_request_bridge.h"

namespace mocktail {
namespace runtime {

// In-experience CoreScripts query the app's cross-experience call state before
// initializing voice. Mocktail does not start or accept cross-experience calls,
// so that state is Idle. This query neither joins voice nor opens a microphone.
class RobloxCallProtocolBridge final {
 public:
  RobloxCallProtocolBridge(JniEnvironmentProvider environment,
                           RobloxMessageBusSymbols symbols,
                           RobloxMessageBusObjects objects);
  Status Initialize();
  Status Shutdown();

 private:
  RobloxMessageBusRequestBridge bridge_;
};

}  // namespace runtime
}  // namespace mocktail
#endif
