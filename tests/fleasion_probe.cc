#include "runtime/fleasion.h"
#include "runtime/runtime_config_file.h"
#include "services/http_client.h"
#include "libc_shim/libc_shim.h"

#include <cstdlib>
#include <iostream>

int main(int argc, char** argv) {
  if (argc != 3) return 2;
  const mocktail::runtime::ProcessEnvironment environment;
  const auto paths = mocktail::runtime::RuntimePaths::FromEnvironment(environment);
  const auto loaded = mocktail::runtime::LoadRuntimeConfig(environment, argv[1]);
  if (!loaded) {
    std::cerr << loaded.error << '\n';
    return 3;
  }
  const auto prepared = mocktail::runtime::PrepareFleasion(loaded.config, environment, paths);
  if (!prepared) {
    std::cerr << prepared.error << '\n';
    return 4;
  }
  std::string error;
  if (!mocktail::runtime::ExportRuntimeConfigEnvironment(loaded.config, &error)) return 5;
  if (!prepared.bundle.empty()) {
    if (setenv("MOCKTAIL_CA_BUNDLE", prepared.bundle.c_str(), 1) != 0) return 6;
    const auto mapped = libc_shim::ConfigureHostCaBundlePathMappings();
    if (!mapped.ok()) return 7;
    FILE* certificate = mocktail_fopen("ssl/cacert.pem", "r");
    if (certificate == nullptr) return 8;
    const int first = fgetc(certificate);
    fclose(certificate);
    if (first != '-') return 9;
  }
  if (std::string(argv[2]) == "prepare") {
    std::cout << prepared.bundle.string() << '\n';
    return 0;
  }
  mocktail::services::CurlHttpClient client;
  mocktail::services::HttpRequest request;
  request.url = argv[2];
  request.timeout_ms = 3000;
  const auto response = client.Get(request);
  if (!response.transport_ok) {
    std::cerr << response.error << '\n';
    return 10;
  }
  std::cout << response.status_code << '\n' << response.body;
  return response.status_code == 200 ? 0 : 11;
}
