#include "runtime/system_proxy.h"

#include <gio/gio.h>

#include <string>
#include <vector>

namespace mocktail {
namespace runtime {
namespace {

constexpr char kSystemProxyProbeUrl[] = "https://www.roblox.com/";

bool IsDirectProxy(const std::string& candidate) {
  return g_ascii_strcasecmp(candidate.c_str(), "direct://") == 0;
}

}  // namespace

SystemProxyResult SelectSystemProxy(
    const std::vector<std::string>& candidates) {
  SystemProxyResult result;
  if (candidates.empty()) {
    result.error = "system proxy resolver returned no connection method";
    return result;
  }

  const std::string& candidate = candidates.front();
  if (IsDirectProxy(candidate)) {
    return result;
  }

  GError* parse_error = nullptr;
  GUri* uri = g_uri_parse(candidate.c_str(), G_URI_FLAGS_SCHEME_NORMALIZE,
                          &parse_error);
  if (uri == nullptr) {
    if (parse_error != nullptr) g_error_free(parse_error);
    result.error = "system proxy resolver returned an invalid proxy URI";
    return result;
  }

  const char* scheme = g_uri_get_scheme(uri);
  const char* host = g_uri_get_host(uri);
  const char* user_info = g_uri_get_userinfo(uri);
  const int port = g_uri_get_port(uri);
  if (user_info != nullptr && user_info[0] != '\0') {
    result.error = "authenticated system proxies are not supported";
  } else if (host == nullptr || port <= 0 || port > 65535) {
    result.error = "system proxy has no valid host and port";
  } else {
    result.proxy = ParseNetworkProxyConfig(
        host, std::to_string(port), scheme != nullptr ? scheme : "");
    if (!result.proxy.has_value()) {
      result.error = "system proxy must use HTTP, HTTPS, or SOCKS5";
    }
  }
  g_uri_unref(uri);
  return result;
}

SystemProxyResult ResolveSystemProxy() {
  SystemProxyResult result;
  GProxyResolver* resolver = g_proxy_resolver_get_default();
  if (resolver == nullptr || !g_proxy_resolver_is_supported(resolver)) {
    result.error = "system proxy resolver is unavailable";
    return result;
  }

  GError* lookup_error = nullptr;
  gchar** resolved = g_proxy_resolver_lookup(
      resolver, kSystemProxyProbeUrl, nullptr, &lookup_error);
  if (resolved == nullptr) {
    result.error = lookup_error != nullptr && lookup_error->message != nullptr
                       ? std::string("could not resolve system proxy: ") +
                             lookup_error->message
                       : "could not resolve system proxy";
    if (lookup_error != nullptr) g_error_free(lookup_error);
    return result;
  }

  std::vector<std::string> candidates;
  for (gchar** candidate = resolved; *candidate != nullptr; ++candidate) {
    candidates.emplace_back(*candidate);
  }
  g_strfreev(resolved);
  return SelectSystemProxy(candidates);
}

}  // namespace runtime
}  // namespace mocktail
