#include "runtime/environment.h"

#include <cstdlib>

namespace mocktail {
namespace runtime {

bool Environment::HasNonEmpty(std::string_view name) const {
  const std::optional<std::string> value = Get(name);
  return value.has_value() && !value->empty();
}

std::string Environment::GetOr(std::string_view name,
                               std::string_view default_value) const {
  const std::optional<std::string> value = Get(name);
  if (!value.has_value() || value->empty()) {
    return std::string(default_value);
  }
  return *value;
}

std::optional<std::string> ProcessEnvironment::Get(
    std::string_view name) const {
  // getenv requires a null-terminated name; std::string also prevents callers
  // from accidentally passing a transient, non-terminated string_view.
  const std::string owned_name(name);
  const char* value = std::getenv(owned_name.c_str());
  if (value == nullptr) {
    return std::nullopt;
  }
  return std::string(value);
}

}  // namespace runtime
}  // namespace mocktail
