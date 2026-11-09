#include "runtime/roblox_call_protocol_bridge.h"

namespace mocktail {
namespace runtime {

RobloxCallProtocolBridge::RobloxCallProtocolBridge(
    JniEnvironmentProvider environment, RobloxMessageBusSymbols symbols,
    RobloxMessageBusObjects objects)
    : bridge_(environment, symbols, objects, "Call", {"getCallState"},
              [](JNIEnv* env, jstring request, std::size_t) {
                // CallProtocolCore registers the wire protocol "Call" (not the
                // Lua module name "CallProtocol"). Its handler ignores the
                // payload and returns CallState::serializeCallStateJson. Match
                // the no-call state; preserve the caller's default-muted
                // initialization path.
                if (request && env->GetStringUTFLength(request) > 65536) {
                  return RobloxMessageBusReply{"{}", 13, "oversized_request"};
                }
                return RobloxMessageBusReply{
                    R"({"status":"Idle","muted":true,"camEnabled":false})", 0,
                    "idle"};
              }) {}

Status RobloxCallProtocolBridge::Initialize() {
  return bridge_.Initialize();
}

Status RobloxCallProtocolBridge::Shutdown() {
  return bridge_.Shutdown();
}

}  // namespace runtime
}  // namespace mocktail
