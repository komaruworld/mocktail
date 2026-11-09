#ifndef MOCKTAIL_RUNTIME_ROBLOX_COOKIE_STORE_H_
#define MOCKTAIL_RUNTIME_ROBLOX_COOKIE_STORE_H_

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace mocktail::runtime {

class Environment;

enum class StoredCookieStatus { kFound, kMissing, kUnavailable };

struct StoredCookie {
  StoredCookieStatus status = StoredCookieStatus::kMissing;
  std::string value;
  std::string error;
  bool from_secret_store = false;
};

class RobloxSecretStore {
 public:
  virtual ~RobloxSecretStore() = default;
  virtual bool enabled() const { return true; }
  virtual StoredCookie Load(const std::filesystem::path& profile);
  virtual bool Store(const std::filesystem::path& profile,
                     const std::string& cookie);
  virtual bool Clear(const std::filesystem::path& profile);
  virtual void Warn(std::string_view message);
};

std::shared_ptr<RobloxSecretStore> OpenRobloxSecretStore();
std::shared_ptr<RobloxSecretStore> OpenRobloxSecretStore(
    const Environment& environment);
void SetRobloxCookieWarningHandler(void (*handler)(std::string_view));

StoredCookie ReadRobloxCookieFile(const std::filesystem::path& path,
                                  bool required = false);

StoredCookie LoadManagedRobloxCookie(const std::filesystem::path& path,
                                     RobloxSecretStore& secrets);
bool RemoveLegacyWebViewCookies(const std::filesystem::path& directory);

}  // namespace mocktail::runtime

#endif  // MOCKTAIL_RUNTIME_ROBLOX_COOKIE_STORE_H_
