#ifdef MOCKTAIL_HAVE_LIBSECRET
#include <libsecret/secret.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstdio>
#include <string>

namespace {
constexpr size_t kMaximumCookieBytes = 1024 * 1024;

void Clear(std::string& value) {
  volatile char* bytes = value.empty() ? nullptr : value.data();
  for (size_t i = 0; i < value.size(); ++i) bytes[i] = '\0';
}
}  // namespace

int main(int argc, char**) {
  if (argc != 1) return 1;
  std::string request;
  std::array<char, 4096> buffer{};
  while (true) {
    const ssize_t count = read(STDIN_FILENO, buffer.data(), buffer.size());
    if (count == 0) break;
    if (count < 0 && errno == EINTR) continue;
    if (count < 0 || request.size() + count > kMaximumCookieBytes + 4099) {
      Clear(request);
      return 1;
    }
    request.append(buffer.data(), static_cast<size_t>(count));
  }
  volatile char* bytes = buffer.data();
  for (size_t i = 0; i < buffer.size(); ++i) bytes[i] = '\0';
  const size_t separator = request.find('\n', 2);
  if (request.size() < 4 || request[1] != '\n' || separator == request.npos ||
      separator == 2 || separator > 4098 ||
      request.find('\0') != request.npos) {
    Clear(request);
    return 1;
  }
  const std::string profile = request.substr(2, separator - 2);
  SecretSchema schema{};
  schema.name = "space.bigrat.mocktail.Cookie";
  schema.flags = SECRET_SCHEMA_NONE;
  schema.attributes[0] = {"profile", SECRET_SCHEMA_ATTRIBUTE_STRING};
  GError* error = nullptr;
  char* password = nullptr;
  char status = 'U';
  switch (request[0]) {
    case 'L':
      password = secret_password_lookup_sync(
          &schema, nullptr, &error, "profile", profile.c_str(), nullptr);
      if (error == nullptr) status = password == nullptr ? 'M' : 'F';
      break;
    case 'S':
      if (separator + 1 < request.size() &&
          secret_password_store_sync(
              &schema, SECRET_COLLECTION_DEFAULT, "Mocktail Roblox session",
              request.c_str() + separator + 1, nullptr, &error, "profile",
              profile.c_str(), nullptr))
        status = 'F';
      break;
    case 'C':
      if (secret_password_clear_sync(&schema, nullptr, &error, "profile",
                                     profile.c_str(), nullptr))
        status = 'F';
      break;
  }
  if (error != nullptr) g_error_free(error);
  Clear(request);
  std::putchar(status);
  if (status == 'F' && password != nullptr) std::fputs(password, stdout);
  if (password != nullptr) secret_password_free(password);
  return std::fflush(stdout) == 0 ? 0 : 1;
}

#else
int main() { return 1; }
#endif
