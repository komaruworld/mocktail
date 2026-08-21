#include "compat/bionic_dns_runtime.h"

#include <netdb.h>

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace mocktail {
namespace compat {
namespace {

constexpr int kBionicAddressInfoFailure = 4;
constexpr int kBionicAddressInfoMemory = 6;

constexpr std::array<std::string_view, 3> kBlockedUploadSuffixes = {
    "crashes.rbxinfra.com",
    "backtrace.rbx.com",
    "backtrace.io",
};

bool EqualAsciiCaseInsensitive(std::string_view left,
                               std::string_view right) noexcept {
  if (left.size() != right.size()) {
    return false;
  }
  for (std::size_t index = 0; index < left.size(); ++index) {
    const auto left_char = static_cast<unsigned char>(left[index]);
    const auto right_char = static_cast<unsigned char>(right[index]);
    if (std::tolower(left_char) != std::tolower(right_char)) {
      return false;
    }
  }
  return true;
}

bool IsHostOrSubdomain(std::string_view host,
                       std::string_view suffix) noexcept {
  if (EqualAsciiCaseInsensitive(host, suffix)) {
    return true;
  }
  if (host.size() <= suffix.size() ||
      host[host.size() - suffix.size() - 1] != '.') {
    return false;
  }
  return EqualAsciiCaseInsensitive(host.substr(host.size() - suffix.size()),
                                   suffix);
}

int BionicAddressInfoError(int host_error) noexcept {
  if (host_error == 0) {
    return 0;
  }
#ifdef EAI_ADDRFAMILY
  if (host_error == EAI_ADDRFAMILY) {
    return 1;
  }
#endif
  if (host_error == EAI_AGAIN) {
    return 2;
  }
  if (host_error == EAI_BADFLAGS) {
    return 3;
  }
  if (host_error == EAI_FAIL) {
    return kBionicAddressInfoFailure;
  }
  if (host_error == EAI_FAMILY) {
    return 5;
  }
  if (host_error == EAI_MEMORY) {
    return kBionicAddressInfoMemory;
  }
#ifdef EAI_NODATA
  if (host_error == EAI_NODATA) {
    return 7;
  }
#endif
  if (host_error == EAI_NONAME) {
    return kBionicAddressInfoNameNotFound;
  }
  if (host_error == EAI_SERVICE) {
    return 9;
  }
  if (host_error == EAI_SOCKTYPE) {
    return 10;
  }
  if (host_error == EAI_SYSTEM) {
    return 11;
  }
#ifdef EAI_OVERFLOW
  if (host_error == EAI_OVERFLOW) {
    return 14;
  }
#endif
  return kBionicAddressInfoFailure;
}

addrinfo HostHints(const BionicAddressInfo* hints) noexcept {
  addrinfo host_hints{};
  if (hints != nullptr) {
    host_hints.ai_flags = hints->ai_flags;
    host_hints.ai_family = hints->ai_family;
    host_hints.ai_socktype = hints->ai_socktype;
    host_hints.ai_protocol = hints->ai_protocol;
  }
  return host_hints;
}

bool CopyHostAddressInfo(const addrinfo* host,
                         BionicAddressInfo** result) noexcept {
  BionicAddressInfo* head = nullptr;
  BionicAddressInfo* tail = nullptr;
  for (const addrinfo* current = host; current != nullptr;
       current = current->ai_next) {
    auto* node = static_cast<BionicAddressInfo*>(
        std::calloc(1, sizeof(BionicAddressInfo)));
    if (node == nullptr) {
      BionicFreeAddressInfo(head);
      return false;
    }
    node->ai_flags = current->ai_flags;
    node->ai_family = current->ai_family;
    node->ai_socktype = current->ai_socktype;
    node->ai_protocol = current->ai_protocol;
    node->ai_addrlen = current->ai_addrlen;
    if (current->ai_canonname != nullptr) {
      node->ai_canonname = ::strdup(current->ai_canonname);
      if (node->ai_canonname == nullptr) {
        std::free(node);
        BionicFreeAddressInfo(head);
        return false;
      }
    }
    if (current->ai_addr != nullptr && current->ai_addrlen > 0) {
      node->ai_addr = static_cast<sockaddr*>(std::malloc(current->ai_addrlen));
      if (node->ai_addr == nullptr) {
        std::free(node->ai_canonname);
        std::free(node);
        BionicFreeAddressInfo(head);
        return false;
      }
      std::memcpy(node->ai_addr, current->ai_addr, current->ai_addrlen);
    }
    if (head == nullptr) {
      head = node;
    } else {
      tail->ai_next = node;
    }
    tail = node;
  }
  *result = head;
  return head != nullptr;
}

}  // namespace

static_assert(sizeof(void*) == 8,
              "the current Bionic DNS runtime supports x86-64 only");
static_assert(sizeof(BionicAddressInfo) == 48,
              "Bionic x86-64 addrinfo must occupy 48 bytes");
static_assert(offsetof(BionicAddressInfo, ai_canonname) == 24);
static_assert(offsetof(BionicAddressInfo, ai_addr) == 32);

bool IsBlockedCrashReportUploadHost(std::string_view host) noexcept {
  while (!host.empty() && host.back() == '.') {
    host.remove_suffix(1);
  }
  for (const std::string_view suffix : kBlockedUploadSuffixes) {
    if (IsHostOrSubdomain(host, suffix)) {
      return true;
    }
  }
  return false;
}

int BionicGetAddressInfo(const char* node, const char* service,
                         const BionicAddressInfo* hints,
                         BionicAddressInfo** result) noexcept {
  if (result == nullptr) {
    return kBionicAddressInfoFailure;
  }
  *result = nullptr;
  if (node != nullptr && IsBlockedCrashReportUploadHost(node)) {
    return kBionicAddressInfoNameNotFound;
  }

  const addrinfo host_hints = HostHints(hints);
  addrinfo* host_result = nullptr;
  const int host_error = ::getaddrinfo(
      node, service, hints != nullptr ? &host_hints : nullptr, &host_result);
  if (host_error != 0) {
    return BionicAddressInfoError(host_error);
  }
  const bool copied = CopyHostAddressInfo(host_result, result);
  ::freeaddrinfo(host_result);
  return copied ? 0 : kBionicAddressInfoMemory;
}

void BionicFreeAddressInfo(BionicAddressInfo* info) noexcept {
  while (info != nullptr) {
    BionicAddressInfo* next = info->ai_next;
    std::free(info->ai_canonname);
    std::free(info->ai_addr);
    std::free(info);
    info = next;
  }
}

hostent* BionicGetHostByName(const char* name) noexcept {
  if (name != nullptr && IsBlockedCrashReportUploadHost(name)) {
    h_errno = HOST_NOT_FOUND;
    return nullptr;
  }
  return ::gethostbyname(name);
}

}  // namespace compat
}  // namespace mocktail

extern "C" int mocktail_bionic_getaddrinfo(
    const char* node, const char* service,
    const mocktail::compat::BionicAddressInfo* hints,
    mocktail::compat::BionicAddressInfo** result) {
  return mocktail::compat::BionicGetAddressInfo(node, service, hints, result);
}

extern "C" void mocktail_bionic_freeaddrinfo(
    mocktail::compat::BionicAddressInfo* info) {
  mocktail::compat::BionicFreeAddressInfo(info);
}

extern "C" hostent* mocktail_bionic_gethostbyname(const char* name) {
  return mocktail::compat::BionicGetHostByName(name);
}
