#ifndef MOCKTAIL_RUNTIME_ENVIRONMENT_H_
#define MOCKTAIL_RUNTIME_ENVIRONMENT_H_

#include <optional>
#include <string>
#include <string_view>

namespace mocktail {
namespace runtime {

// Read-only environment abstraction. Production code uses ProcessEnvironment;
// tests can provide a deterministic map-backed implementation.
class Environment {
 public:
  virtual ~Environment() = default;

  virtual std::optional<std::string> Get(std::string_view name) const = 0;

  bool HasNonEmpty(std::string_view name) const;
  std::string GetOr(std::string_view name,
                    std::string_view default_value) const;
};

class ProcessEnvironment final : public Environment {
 public:
  std::optional<std::string> Get(std::string_view name) const override;
};

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_ENVIRONMENT_H_
