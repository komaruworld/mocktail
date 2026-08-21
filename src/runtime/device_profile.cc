#include "runtime/device_profile.h"

#include <array>
#include <cstdint>

namespace mocktail {
namespace runtime {
namespace {

const std::array<DeviceProfile, 3> kProfiles = {{
    {
        "pc-windows-11",
        "pc-windows-11",
        DeviceClass::kPc,
        "Windows",
        "Windows 11 PC",
        "Microsoft",
        "Windows 11 PC",
        "Microsoft",
        "windows11",
        "windows-11-x86_64",
        "x86_64",
        std::string(kRobloxDesktopHttpUserAgent),
        false,
        true,
        true,
        true,
    },
    {
        "mobile-pixel-7",
        "mobile-pixel-7",
        DeviceClass::kMobile,
        "Android",
        "Google Pixel 7",
        "Google",
        "Pixel 7",
        "google",
        "panther",
        "panther",
        "Google Tensor G2",
        {},
        true,
        false,
        false,
        false,
    },
    {
        "console-ps5",
        "console-ps5",
        DeviceClass::kConsole,
        "PlayStation",
        "PlayStation 5",
        "Sony Interactive Entertainment",
        "PlayStation 5",
        "PlayStation",
        "prospero",
        "ps5",
        "AMD custom Zen 2 / RDNA 2",
        std::string(kRobloxConsoleAdmissionUserAgent),
        false,
        true,
        true,
        false,
    },
}};

std::string_view CanonicalName(std::string_view name) {
  if (name == "pc") {
    return "pc-windows-11";
  }
  if (name == "mobile") {
    return "mobile-pixel-7";
  }
  if (name == "console" || name == "console-xbox-series-x") {
    return "console-ps5";
  }
  return name;
}

}  // namespace

const DeviceProfile* FindDeviceProfile(std::string_view name) {
  const std::string_view canonical = CanonicalName(name);
  for (const DeviceProfile& profile : kProfiles) {
    if (profile.name == canonical) {
      return &profile;
    }
  }
  return nullptr;
}

bool IsValidDeviceProfileValue(std::string_view value, std::size_t maximum) {
  if (value.empty() || value.size() > maximum) {
    return false;
  }
  for (const unsigned char character : value) {
    if (character < 0x20U || character == 0x7fU) {
      return false;
    }
  }
  return true;
}

std::string BuildCustomDeviceProfileCacheKey(const DeviceProfile& profile) {
  std::uint64_t hash = UINT64_C(14695981039346656037);
  const auto consume = [&hash](std::string_view value) {
    for (const unsigned char character : value) {
      hash ^= character;
      hash *= UINT64_C(1099511628211);
    }
    hash ^= UINT64_C(0xff);
    hash *= UINT64_C(1099511628211);
  };
  consume(profile.name);
  consume(profile.platform_name);
  consume(profile.display_name);
  consume(profile.manufacturer);
  consume(profile.model);
  consume(profile.brand);
  consume(profile.device_code);
  consume(profile.device_sku);
  consume(profile.soc_model);

  constexpr char kHex[] = "0123456789abcdef";
  std::array<char, 16> encoded{};
  for (std::size_t index = 0; index < encoded.size(); ++index) {
    const std::size_t shift = (encoded.size() - index - 1) * 4;
    encoded[index] = kHex[(hash >> shift) & 0xfU];
  }
  return profile.name + "-custom-" +
         std::string(encoded.data(), encoded.size());
}

std::string_view DeviceClassName(DeviceClass device_class) {
  switch (device_class) {
    case DeviceClass::kPc:
      return "pc";
    case DeviceClass::kMobile:
      return "mobile";
    case DeviceClass::kConsole:
      return "console";
  }
  return "unknown";
}

}  // namespace runtime
}  // namespace mocktail
