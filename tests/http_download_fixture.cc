#include <cstdlib>
#include <iostream>

#include "update/http_download.h"

int main(int argc, char** argv) {
  if (argc != 5) return 2;
  mocktail::update::HttpTransferRequest request;
  request.url = argv[1];
  request.allowed_hosts = {"download.test"};
  request.ca_bundle = argv[3];
  request.maximum_bytes = 1024 * 1024;
  request.maximum_attempts = std::atoi(argv[4]);
  request.retry_delay_ms = 0;
  request.connect_timeout_ms = 2000;
  request.transfer_timeout_ms = 3000;
  const auto result = mocktail::update::DownloadFile(request, argv[2]);
  if (!result) {
    std::cerr << result.error << '\n';
    return 1;
  }
  std::cout << result.sha256 << '\n';
}
