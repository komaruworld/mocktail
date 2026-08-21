#include "runtime/webview_helper_launcher.h"

#include <gtest/gtest.h>
#include <signal.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mocktail {
namespace runtime {
namespace {

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    char pattern[] = "/tmp/mocktail_webview_launcher_XXXXXX";
    char *created = mkdtemp(pattern);
    if (created != nullptr) {
      path_ = created;
    }
  }
  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

bool WriteExecutable(const std::filesystem::path &path,
                     const std::string &body) {
  std::ofstream output(path);
  output << body;
  output.close();
  return output.good() && chmod(path.c_str(), 0700) == 0;
}

std::string ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input),
                     std::istreambuf_iterator<char>());
}

bool WaitForFile(const std::filesystem::path &path,
                 std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (std::filesystem::is_regular_file(path)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return std::filesystem::is_regular_file(path);
}

bool WaitForProcessToBeReaped(pid_t process_id,
                              std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    errno = 0;
    if (kill(process_id, 0) < 0 && errno == ESRCH) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  errno = 0;
  return kill(process_id, 0) < 0 && errno == ESRCH;
}

struct ExitProbe {
  std::mutex mutex;
  std::condition_variable condition;
  int exits = 0;
};

void RecordExit(void *context) {
  auto *probe = static_cast<ExitProbe *>(context);
  std::lock_guard<std::mutex> lock(probe->mutex);
  ++probe->exits;
  probe->condition.notify_all();
}

bool WaitForExit(const std::shared_ptr<ExitProbe> &probe,
                 std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(probe->mutex);
  return probe->condition.wait_for(lock, timeout,
                                   [probe] { return probe->exits != 0; });
}

TEST(WebViewHelperLauncherTest, AcceptsOnlyHttpsRobloxOrigins) {
  std::string error;
  EXPECT_TRUE(
      ValidateWebViewUrl("https://www.roblox.com/login?returnUrl=%2F", &error));
  EXPECT_TRUE(ValidateWebViewUrl("https://authorize.roblox.com/", &error));
  std::string long_auth_url = "https://www.roblox.com/login?state=";
  long_auth_url.append(8 * 1024, 'a');
  EXPECT_TRUE(ValidateWebViewUrl(long_auth_url, &error)) << error;
  long_auth_url.append(kMaximumWebViewUrlBytes, 'b');
  EXPECT_FALSE(ValidateWebViewUrl(long_auth_url, &error));
  for (const char *rejected : {
           "http://www.roblox.com/login",
           "https://evilroblox.com/login",
           "https://roblox.com.example.org/login",
           "https://user@roblox.com/login",
           "https://roblox.com:444/login",
           "https://roblox.com/login\nhttps://example.org",
           "https://www.roblox.com/games/../login",
           "https://www.roblox.com/games/%2e%2e/login",
           "https://www.roblox.com/games/%2Fetc",
           "https://www.roblox.com/games/%5cetc",
       }) {
    error.clear();
    EXPECT_FALSE(ValidateWebViewUrl(rejected, &error)) << rejected;
    EXPECT_FALSE(error.empty());
  }
}

TEST(WebViewHelperLauncherTest, NormalizesInternalRoutesToFixedRobloxOrigin) {
  std::string normalized;
  std::string error;
  EXPECT_TRUE(NormalizeWebViewUrl("www:games/servers-section/987654321",
                                  &normalized, &error))
      << error;
  EXPECT_EQ(normalized,
            "https://www.roblox.com/games/servers-section/987654321");
  EXPECT_TRUE(
      NormalizeWebViewUrl("www:login?returnUrl=%2Fhome", &normalized, &error));
  EXPECT_EQ(normalized, "https://www.roblox.com/login?returnUrl=%2Fhome");
  EXPECT_TRUE(
      NormalizeWebViewUrl("www:games/other-section/123", &normalized, &error));
  EXPECT_EQ(normalized, "https://www.roblox.com/games/other-section/123");

  for (const char* rejected : {
           "www:",
           "www:/login",
           "www:games/servers-section/../login",
           "www:games/servers-section/%2e%2e",
       }) {
    normalized.clear();
    error.clear();
    EXPECT_FALSE(NormalizeWebViewUrl(rejected, &normalized, &error))
        << rejected;
    EXPECT_TRUE(normalized.empty()) << rejected;
    EXPECT_FALSE(error.empty()) << rejected;
  }
}

TEST(WebViewHelperLauncherTest, DecodesBoundedStdinProtocol) {
  const std::string request =
      "MOCKTAIL-WEBVIEW 1\n28\nhttps://www.roblox.com/login";
  std::string decoded;
  std::string error;
  EXPECT_TRUE(DecodeWebViewRequest(request, &decoded, &error)) << error;
  EXPECT_EQ(decoded, "https://www.roblox.com/login");
  EXPECT_FALSE(DecodeWebViewRequest(
      "MOCKTAIL-WEBVIEW 1\n999\nhttps://www.roblox.com/login", &decoded,
      &error));
}

TEST(WebViewHelperLauncherTest, EncodesDistinctBoundedHybridEvents) {
  std::string packet;
  ASSERT_TRUE(
      EncodeWebViewHelperEventPacket(WebViewHelperEventType::kExecuteRoblox,
                                     R"({"command":"open"})", &packet));
  WebViewHelperEvent event;
  std::string error;
  ASSERT_TRUE(DecodeWebViewHelperEventPacket(packet, &event, &error)) << error;
  EXPECT_EQ(event.type, WebViewHelperEventType::kExecuteRoblox);
  EXPECT_EQ(event.payload, R"({"command":"open"})");

  ASSERT_TRUE(
      EncodeWebViewHelperEventPacket(WebViewHelperEventType::kRobloxWkHybrid,
                                     R"({"event":"captchaSuccess"})", &packet));
  ASSERT_TRUE(DecodeWebViewHelperEventPacket(packet, &event, &error)) << error;
  EXPECT_EQ(event.type, WebViewHelperEventType::kRobloxWkHybrid);
  EXPECT_EQ(event.payload, R"({"event":"captchaSuccess"})");

  ASSERT_TRUE(EncodeWebViewHelperEventPacket(WebViewHelperEventType::kReady, {},
                                             &packet));
  ASSERT_TRUE(DecodeWebViewHelperEventPacket(packet, &event, &error)) << error;
  EXPECT_EQ(event.type, WebViewHelperEventType::kReady);
  EXPECT_TRUE(event.payload.empty());
}

TEST(WebViewHelperLauncherTest, WaitsForExplicitSurfaceReadiness) {
  TemporaryDirectory temporary;
  ASSERT_FALSE(temporary.path().empty());
  const std::filesystem::path helper = temporary.path() / "fake-helper";
  ASSERT_TRUE(WriteExecutable(
      helper,
      "#!/bin/sh\n"
      "python3 -c 'import socket,time; s=socket.socket(fileno=198); "
      "s.send(b\"MWVE\"+bytes([1,3,0,0])+bytes(4)); time.sleep(1)'\n"));

  const WebViewHelperLaunchResult result =
      LaunchWebViewHelper(helper, "https://www.roblox.com/login");
  ASSERT_TRUE(result) << result.error;
  EXPECT_TRUE(result.process->WaitUntilReady(std::chrono::seconds(1)));
  EXPECT_TRUE(result.process->WaitUntilReady(std::chrono::milliseconds(0)));
  EXPECT_TRUE(result.process->RequestClose());
}

TEST(WebViewHelperLauncherTest, RejectsMalformedOrOversizedHybridEvents) {
  std::string packet;
  EXPECT_FALSE(EncodeWebViewHelperEventPacket(
      WebViewHelperEventType::kExecuteRoblox, {}, &packet));
  EXPECT_FALSE(EncodeWebViewHelperEventPacket(
      WebViewHelperEventType::kExecuteRoblox,
      std::string(kMaximumWebViewHybridEventBytes + 1, 'x'), &packet));
  ASSERT_TRUE(EncodeWebViewHelperEventPacket(
      WebViewHelperEventType::kExecuteRoblox, "{}", &packet));

  WebViewHelperEvent event;
  std::string error;
  packet[5] = static_cast<char>(99);
  EXPECT_FALSE(DecodeWebViewHelperEventPacket(packet, &event, &error));
  EXPECT_TRUE(event.payload.empty());
  packet[5] = static_cast<char>(WebViewHelperEventType::kExecuteRoblox);
  packet[11] = 3;
  EXPECT_FALSE(DecodeWebViewHelperEventPacket(packet, &event, &error));
}

TEST(WebViewHelperLauncherTest, DrainsChildEventsWithoutBlocking) {
  TemporaryDirectory temporary;
  ASSERT_FALSE(temporary.path().empty());
  const std::filesystem::path helper = temporary.path() / "fake-helper";
  ASSERT_TRUE(WriteExecutable(
      helper,
      "#!/bin/sh\n"
      "python3 -c 'import socket,time; s=socket.socket(fileno=198); "
      "p=b\"{\\\"event\\\":\\\"ready\\\"}\"; "
      "s.send(b\"MWVE\"+bytes([1,2,0,0])+len(p).to_bytes(4,\"big\")+p); "
      "time.sleep(1)'\n"));

  const WebViewHelperLaunchResult result =
      LaunchWebViewHelper(helper, "https://www.roblox.com/login");
  ASSERT_TRUE(result) << result.error;
  std::vector<WebViewHelperEvent> events;
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (events.empty() && std::chrono::steady_clock::now() < deadline) {
    ASSERT_TRUE(result.process->DrainEvents(&events));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].type, WebViewHelperEventType::kRobloxWkHybrid);
  EXPECT_EQ(events[0].payload, R"({"event":"ready"})");
  const auto started = std::chrono::steady_clock::now();
  ASSERT_TRUE(result.process->DrainEvents(&events));
  EXPECT_LT(std::chrono::steady_clock::now() - started,
            std::chrono::milliseconds(100));
  EXPECT_TRUE(result.process->RequestClose());
}

TEST(WebViewHelperLauncherTest, SendsUrlOnStdinWithoutArguments) {
  TemporaryDirectory temporary;
  ASSERT_FALSE(temporary.path().empty());
  const std::filesystem::path helper = temporary.path() / "fake-helper";
  const std::filesystem::path marker = temporary.path() / "request-ok";
  ASSERT_TRUE(
      WriteExecutable(helper,
                      "#!/bin/sh\n"
                      "[ \"$#\" -eq 0 ] || exit 11\n"
                      "request=$(cat) || exit 12\n"
                      "[ \"$request\" = 'MOCKTAIL-WEBVIEW "
                      "1\n28\nhttps://www.roblox.com/login' ] || exit 13\n"
                      "touch '" +
                          marker.string() + "'\n"));

  auto exit_probe = std::make_shared<ExitProbe>();
  const WebViewHelperLaunchResult result =
      LaunchWebViewHelper(helper, "https://www.roblox.com/login",
                          WebViewHelperExitObserver{exit_probe, &RecordExit});
  EXPECT_TRUE(result) << result.error;
  EXPECT_TRUE(result.spawned);
  EXPECT_GT(result.process_id, 0);
  EXPECT_TRUE(WaitForFile(marker, std::chrono::seconds(2)));
  EXPECT_TRUE(
      WaitForProcessToBeReaped(result.process_id, std::chrono::seconds(2)));
  EXPECT_TRUE(WaitForExit(exit_probe, std::chrono::seconds(2)));
  EXPECT_EQ(exit_probe->exits, 1);
}

TEST(WebViewHelperLauncherTest, SendsNormalizedInternalRouteToHelper) {
  TemporaryDirectory temporary;
  ASSERT_FALSE(temporary.path().empty());
  const std::filesystem::path helper = temporary.path() / "fake-helper";
  const std::filesystem::path output = temporary.path() / "request";
  ASSERT_TRUE(
      WriteExecutable(helper, "#!/bin/sh\ncat > '" + output.string() + "'\n"));

  const WebViewHelperLaunchResult result =
      LaunchWebViewHelper(helper, "www:games/servers-section/987654321");
  ASSERT_TRUE(result) << result.error;
  ASSERT_TRUE(WaitForFile(output, std::chrono::seconds(2)));

  std::ifstream input(output);
  const std::string request((std::istreambuf_iterator<char>(input)),
                            std::istreambuf_iterator<char>());
  const std::string canonical =
      "https://www.roblox.com/games/servers-section/987654321";
  EXPECT_EQ(request, "MOCKTAIL-WEBVIEW 1\n" + std::to_string(canonical.size()) +
                         "\n" + canonical);
}

TEST(WebViewHelperLauncherTest, ReturnsWithoutWaitingForHelperWindow) {
  TemporaryDirectory temporary;
  ASSERT_FALSE(temporary.path().empty());
  const std::filesystem::path helper = temporary.path() / "fake-helper";
  ASSERT_TRUE(WriteExecutable(helper, "#!/bin/sh\nsleep 2\n"));

  const auto started_at = std::chrono::steady_clock::now();
  const WebViewHelperLaunchResult result =
      LaunchWebViewHelper(helper, "https://www.roblox.com/login");
  const auto elapsed = std::chrono::steady_clock::now() - started_at;

  EXPECT_TRUE(result) << result.error;
  EXPECT_LT(elapsed, std::chrono::milliseconds(500));
  EXPECT_TRUE(
      WaitForProcessToBeReaped(result.process_id, std::chrono::seconds(3)));
}

TEST(WebViewHelperLauncherTest, OwnsAndClosesTheExactSpawnedProcess) {
  TemporaryDirectory temporary;
  ASSERT_FALSE(temporary.path().empty());
  const std::filesystem::path helper = temporary.path() / "fake-helper";
  const std::filesystem::path close_packet = temporary.path() / "close-packet";
  const std::filesystem::path ready = temporary.path() / "ready";
  ASSERT_TRUE(WriteExecutable(helper,
                              "#!/bin/sh\n"
                              "cat >/dev/null\n"
                              "touch '" +
                                  ready.string() +
                                  "'\n"
                                  "dd bs=65548 count=1 status=none <&198 >'" +
                                  close_packet.string() + "'\n"));

  const WebViewHelperLaunchResult result =
      LaunchWebViewHelper(helper, "https://www.roblox.com/login");
  ASSERT_TRUE(result) << result.error;
  ASSERT_NE(result.process, nullptr);
  EXPECT_TRUE(result.process->running());
  EXPECT_EQ(result.process->process_id(), result.process_id);
  ASSERT_TRUE(WaitForFile(ready, std::chrono::seconds(2)));
  EXPECT_TRUE(result.process->RequestClose());
  EXPECT_TRUE(
      WaitForProcessToBeReaped(result.process_id, std::chrono::seconds(2)));
  EXPECT_FALSE(result.process->running());
  WebViewHelperControlCommand command;
  std::string error;
  ASSERT_TRUE(DecodeWebViewHelperControlPacket(ReadFile(close_packet), &command,
                                               &error))
      << error;
  EXPECT_EQ(command.operation, WebViewHelperControlOperation::kClose);
  EXPECT_TRUE(command.payload.empty());
}

TEST(WebViewHelperLauncherTest, SendsBoundedTypedControlsToRunningHelper) {
  TemporaryDirectory temporary;
  ASSERT_FALSE(temporary.path().empty());
  const std::filesystem::path helper = temporary.path() / "fake-helper";
  const std::filesystem::path ready = temporary.path() / "ready";
  const std::string packet_prefix = (temporary.path() / "packet-").string();
  ASSERT_TRUE(WriteExecutable(helper,
                              "#!/bin/sh\n"
                              "cat >/dev/null\n"
                              "touch '" +
                                  ready.string() +
                                  "'\n"
                                  "index=0\n"
                                  "while [ \"$index\" -lt 10 ]; do\n"
                                  "  dd bs=65548 count=1 status=none <&198 >'" +
                                  packet_prefix +
                                  "'\"$index\" || exit 21\n"
                                  "  index=$((index + 1))\n"
                                  "done\n"));

  const WebViewHelperLaunchResult result =
      LaunchWebViewHelper(helper, "https://www.roblox.com/login");
  ASSERT_TRUE(result) << result.error;
  ASSERT_NE(result.process, nullptr);
  ASSERT_TRUE(WaitForFile(ready, std::chrono::seconds(2)));
  EXPECT_FALSE(result.process->LoadUrl("https://example.org/phishing"));
  EXPECT_FALSE(result.process->SetTitle(
      std::string(kMaximumWebViewTitleBytes + 1, 'x')));
  EXPECT_FALSE(result.process->EvaluateJavaScript(
      std::string(kMaximumWebViewJavaScriptBytes + 1, 'x')));
  EXPECT_TRUE(
      result.process->LoadUrl("www:captcha/app/login?hybrid-return-token=1"));
  EXPECT_TRUE(result.process->SetTitle("Roblox Login"));
  EXPECT_TRUE(result.process->SetVisible(false));
  EXPECT_TRUE(result.process->EvaluateJavaScript("document.title = 'Roblox';"));
  EXPECT_TRUE(result.process->SetBackNavigationDisabled(true));
  EXPECT_TRUE(result.process->SetShowDomainAsTitle(true));
  EXPECT_TRUE(result.process->SetRobloxCookie("_|signed-token"));
  EXPECT_TRUE(result.process->SetRobloxCookie({}));
  EXPECT_TRUE(result.process->ClearRobloxCookie());
  EXPECT_TRUE(result.process->RequestClose());
  ASSERT_TRUE(
      WaitForProcessToBeReaped(result.process_id, std::chrono::seconds(2)));

  const std::vector<WebViewHelperControlOperation> expected_operations = {
      WebViewHelperControlOperation::kLoadUrl,
      WebViewHelperControlOperation::kSetTitle,
      WebViewHelperControlOperation::kSetVisible,
      WebViewHelperControlOperation::kEvaluateJavaScript,
      WebViewHelperControlOperation::kSetBackNavigationDisabled,
      WebViewHelperControlOperation::kSetShowDomainAsTitle,
      WebViewHelperControlOperation::kSetRobloxCookie,
      WebViewHelperControlOperation::kRetainPersistentRobloxCookie,
      WebViewHelperControlOperation::kClearRobloxCookie,
      WebViewHelperControlOperation::kClose,
  };
  const std::vector<std::string> expected_payloads = {
      "https://www.roblox.com/captcha/app/login?hybrid-return-token=1",
      "Roblox Login",
      std::string(1, '\0'),
      "document.title = 'Roblox';",
      std::string(1, '\1'),
      std::string(1, '\1'),
      "_|signed-token",
      "",
      "",
      "",
  };
  for (std::size_t index = 0; index < expected_operations.size(); ++index) {
    WebViewHelperControlCommand command;
    std::string error;
    ASSERT_TRUE(DecodeWebViewHelperControlPacket(
        ReadFile(packet_prefix + std::to_string(index)), &command, &error))
        << "packet " << index << ": " << error;
    EXPECT_EQ(command.operation, expected_operations[index]);
    EXPECT_EQ(command.payload, expected_payloads[index]);
  }

  EXPECT_FALSE(result.process->LoadUrl("https://www.roblox.com/home"));
  EXPECT_FALSE(result.process->SetTitle("closed"));
  EXPECT_FALSE(result.process->SetVisible(true));
  EXPECT_FALSE(result.process->EvaluateJavaScript("1 + 1"));
  EXPECT_FALSE(result.process->SetBackNavigationDisabled(false));
  EXPECT_FALSE(result.process->SetShowDomainAsTitle(false));
  EXPECT_TRUE(result.process->RequestClose());
}

TEST(WebViewHelperLauncherTest, RejectsMalformedAndOversizedControlPackets) {
  WebViewHelperControlCommand command;
  std::string error;
  EXPECT_FALSE(DecodeWebViewHelperControlPacket("MWVC", &command, &error));
  EXPECT_FALSE(error.empty());

  std::string packet(12, '\0');
  packet.replace(0, 4, "MWVC");
  packet[4] = '\1';
  packet[5] = static_cast<char>(WebViewHelperControlOperation::kClose);
  EXPECT_TRUE(DecodeWebViewHelperControlPacket(packet, &command, &error))
      << error;
  packet[11] = '\1';
  EXPECT_FALSE(DecodeWebViewHelperControlPacket(packet, &command, &error));

  std::string oversized(kMaximumWebViewControlPacketBytes + 1, 'x');
  EXPECT_FALSE(DecodeWebViewHelperControlPacket(oversized, &command, &error));
}

TEST(WebViewHelperLauncherTest, RejectsInvalidUrlBeforeSpawn) {
  const WebViewHelperLaunchResult result = LaunchWebViewHelper(
      "/definitely/missing/helper", "https://example.org/login");
  EXPECT_FALSE(result);
  EXPECT_FALSE(result.spawned);
  EXPECT_NE(result.error.find("roblox.com"), std::string::npos);
}

} // namespace
} // namespace runtime
} // namespace mocktail
