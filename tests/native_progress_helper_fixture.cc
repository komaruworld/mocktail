#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cstdlib>
#include <fstream>
#include <string_view>

int main(int argc, char** argv) {
  if (argc != 2 || std::string_view(argv[1]) != "--progress-monitor") {
    return 64;
  }
  const char* capture = std::getenv("MOCKTAIL_TEST_PROGRESS_CAPTURE");
  if (capture == nullptr || capture[0] == '\0') return 65;
  std::ofstream output(capture);
  if (!output) return 66;
  std::array<char, 4096> packet{};
  while (true) {
    const ssize_t size = recv(STDIN_FILENO, packet.data(), packet.size(), 0);
    if (size <= 0) return size == 0 ? 0 : 67;
    output.write(packet.data(), size);
    output << '\n';
    output.flush();
    if (packet[0] == 'S') return 0;
  }
}
