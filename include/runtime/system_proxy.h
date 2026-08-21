#ifndef MOCKTAIL_RUNTIME_SYSTEM_PROXY_H_
#define MOCKTAIL_RUNTIME_SYSTEM_PROXY_H_

#include <optional>
#include <string>
#include <vector>

#include "runtime/runtime_config.h"

namespace mocktail {
namespace runtime {

struct SystemProxyResult {
  std::optional<NetworkProxyConfig> proxy;
  std::string error;

  explicit operator bool() const { return error.empty(); }
};

// Selects the first proxy returned by the host resolver. An empty `proxy`
// means that the resolver selected a direct connection.
SystemProxyResult SelectSystemProxy(
    const std::vector<std::string>& candidates);

// Resolves the host's proxy policy for Roblox HTTPS traffic. GIO delegates to
// the desktop/environment proxy resolver, including PAC when it is available.
// HTTP/HTTPS resolver results and SOCKS5 proxy endpoints are supported.
SystemProxyResult ResolveSystemProxy();

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_SYSTEM_PROXY_H_
