#include "runtime/failure_dialog.h"

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mocktail {
namespace runtime {
namespace {

class MapEnvironment final : public Environment {
 public:
  explicit MapEnvironment(std::unordered_map<std::string, std::string> values)
      : values_(std::move(values)) {}

  std::optional<std::string> Get(std::string_view name) const override {
    const auto found = values_.find(std::string(name));
    return found == values_.end() ? std::nullopt
                                  : std::optional<std::string>(found->second);
  }

 private:
  std::unordered_map<std::string, std::string> values_;
};

class FakeDialogHelper final {
 public:
  FakeDialogHelper() {
    char pattern[] = "/tmp/mocktail_failure_dialog_XXXXXX";
    const char* created = mkdtemp(pattern);
    if (created == nullptr) {
      return;
    }
    root_ = created;
    helper_ = root_ / "helper.py";
    capture_ = root_ / "capture";
    std::ofstream output(helper_);
    output << "#!/usr/bin/python3\n"
              "import socket\n"
              "import sys\n"
              "capture = '"
           << capture_.string()
           << "'\n"
              "if len(sys.argv) == 3 and sys.argv[1] in "
              "('--message', '--warning'):\n"
              "    with open(capture, 'a', encoding='utf-8') as stream:\n"
              "        stream.write(sys.argv[1] + ':' + sys.argv[2] + "
              "'\\n')\n"
              "    raise SystemExit(0)\n"
              "channel = socket.socket(fileno=0)\n"
              "while True:\n"
              "    packet = channel.recv(4096)\n"
              "    if not packet:\n"
              "        break\n"
              "    with open(capture, 'a', encoding='utf-8') as stream:\n"
              "        stream.write(packet.decode('utf-8') + '\\n')\n"
              "    if packet[:1] in (b'S', b'F'):\n"
              "        break\n";
    output.close();
    std::filesystem::permissions(helper_, std::filesystem::perms::owner_all,
                                 std::filesystem::perm_options::replace);
  }

  ~FakeDialogHelper() {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  const std::filesystem::path& helper() const { return helper_; }
  const std::filesystem::path& capture() const { return capture_; }

 private:
  std::filesystem::path root_;
  std::filesystem::path helper_;
  std::filesystem::path capture_;
};

std::vector<std::string> ReadLines(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::vector<std::string> lines;
  for (std::string line; std::getline(input, line);) {
    lines.push_back(line);
  }
  return lines;
}

MapEnvironment DialogEnvironment(const FakeDialogHelper& helper) {
  return MapEnvironment({
      {"DISPLAY", ":test"},
      {"MOCKTAIL_FAILURE_DIALOG_HELPER", helper.helper().string()},
  });
}

TEST(FailureDialogTest, DisablesDialogsForCanaryAndHeadlessLaunches) {
  FakeDialogHelper helper;
  const MapEnvironment canary({
      {"DISPLAY", ":test"},
      {"MOCKTAIL_FAILURE_DIALOG_HELPER", helper.helper().string()},
      {"MOCKTAIL_ISOLATED_CANARY", "1"},
  });
  const MapEnvironment headless({
      {"DISPLAY", ":test"},
      {"MOCKTAIL_FAILURE_DIALOG_HELPER", helper.helper().string()},
      {"MOCKTAIL_HEADLESS", "1"},
  });
  const MapEnvironment explicitly_disabled({
      {"DISPLAY", ":test"},
      {"MOCKTAIL_FAILURE_DIALOG_HELPER", helper.helper().string()},
      {"MOCKTAIL_DISABLE_FAILURE_DIALOG", "1"},
  });
  const MapEnvironment no_display(
      {{"MOCKTAIL_FAILURE_DIALOG_HELPER", helper.helper().string()}});

  EXPECT_FALSE(FailureDialogsEnabled(canary));
  EXPECT_FALSE(FailureDialogMonitor::Start(canary, "ignored").active());
  EXPECT_FALSE(ShowFailureDialog(canary, "ignored"));
  EXPECT_FALSE(FailureDialogsEnabled(headless));
  EXPECT_FALSE(FailureDialogsEnabled(explicitly_disabled));
  EXPECT_FALSE(FailureDialogsEnabled(no_display));
}

TEST(FailureDialogTest, OneShotPassesAUserFacingMessage) {
  FakeDialogHelper helper;
  const MapEnvironment environment = DialogEnvironment(helper);

  ASSERT_TRUE(ShowFailureDialog(environment, "Already running."));
  ASSERT_TRUE(ShowWarningDialog(environment, "Sign in again."));
  EXPECT_EQ(ReadLines(helper.capture()),
            std::vector<std::string>({"--message:Already running.",
                                      "--warning:Sign in again."}));
}

TEST(FailureDialogTest, MonitorDismissesAfterSuccessfulShutdown) {
  FakeDialogHelper helper;
  const MapEnvironment environment = DialogEnvironment(helper);
  FailureDialogMonitor monitor =
      FailureDialogMonitor::Start(environment, "Startup failed.");
  ASSERT_TRUE(monitor.active());

  monitor.SetMessage("Runtime failed.");
  monitor.MarkSuccessful();

  EXPECT_EQ(
      ReadLines(helper.capture()),
      std::vector<std::string>({"MStartup failed.", "MRuntime failed.", "S"}));
}

TEST(FailureDialogTest, MonitorReportsNormalFailureOnScopeExit) {
  FakeDialogHelper helper;
  const MapEnvironment environment = DialogEnvironment(helper);
  {
    FailureDialogMonitor monitor =
        FailureDialogMonitor::Start(environment, "Startup failed.");
    ASSERT_TRUE(monitor.active());
    monitor.SetMessage("Runtime failed.");
  }

  EXPECT_EQ(
      ReadLines(helper.capture()),
      std::vector<std::string>({"MStartup failed.", "MRuntime failed.", "F"}));
}

}  // namespace
}  // namespace runtime
}  // namespace mocktail
