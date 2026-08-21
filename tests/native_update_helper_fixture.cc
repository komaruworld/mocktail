#include <sys/socket.h>
#include <unistd.h>

#include <charconv>
#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>

namespace {

const char* Environment(const char* name) {
  const char* value = std::getenv(name);
  return value == nullptr ? "" : value;
}

int IntegerEnvironment(const char* name, int fallback) {
  const std::string_view value(Environment(name));
  int parsed = fallback;
  const auto result =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  return result.ec == std::errc() && result.ptr == value.data() + value.size()
             ? parsed
             : fallback;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 3 || std::string_view(argv[1]) != "update" ||
      std::string_view(argv[2]) != "--startup-preflight") {
    return 64;
  }
  const char* capture = Environment("MOCKTAIL_TEST_UPDATE_CAPTURE");
  if (capture[0] != '\0') {
    std::ofstream output(capture);
    output << Environment("MOCKTAIL_CONFIG_FILE") << '\n'
           << Environment("MOCKTAIL_DATA_ROOT") << '\n'
           << Environment("MOCKTAIL_CACHE_ROOT") << '\n'
           << Environment("MOCKTAIL_STATE_ROOT") << '\n';
    if (!output) return 65;
  }
  const int progress = IntegerEnvironment("MOCKTAIL_UPDATE_PROGRESS_FD", -1);
  if (progress >= 0) {
    constexpr std::string_view packet = "PDownloading Roblox...";
    if (send(progress, packet.data(), packet.size(), MSG_NOSIGNAL) < 0) {
      return 66;
    }
  }
  return IntegerEnvironment("MOCKTAIL_TEST_UPDATE_EXIT", 0);
}
