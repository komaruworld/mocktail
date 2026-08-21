#ifndef MOCKTAIL_RUNTIME_ROBLOX_PLATFORM_WEB_SYMBOLS_H_
#define MOCKTAIL_RUNTIME_ROBLOX_PLATFORM_WEB_SYMBOLS_H_

#include "runtime/roblox_browser_service_bridge.h"
#include "runtime/roblox_web_view_bridge.h"

namespace mocktail {
namespace runtime {

// Exported web-platform entrypoints resolved as one capability. Keeping
// this Build-ID-sensitive symbol inventory outside the research runtime lets
// the composition fail closed when a future payload changes any contract.
struct RobloxPlatformWebSymbols {
  RobloxWebViewMessageBusSymbols web_view;
  RobloxBrowserServiceSymbols browser_service;

  bool complete() const {
    return web_view.complete() && browser_service.complete();
  }
};

RobloxPlatformWebSymbols ResolveRobloxPlatformWebSymbols(
    void* roblox_library, SubscribeWebViewRawFn subscribe_raw,
    DeleteWebViewConnectionFn delete_connection);

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_ROBLOX_PLATFORM_WEB_SYMBOLS_H_
