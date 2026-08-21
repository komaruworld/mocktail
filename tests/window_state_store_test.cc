#include "window/window_state_store.h"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>

namespace mocktail {
namespace window {
namespace {

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    char pattern[] = "/tmp/mocktail_window_state_XXXXXX";
    char* created = mkdtemp(pattern);
    if (created != nullptr) {
      path_ = created;
    }
  }
  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

mode_t FileMode(const std::filesystem::path& path) {
  struct stat status{};
  return stat(path.c_str(), &status) == 0 ? status.st_mode & 0777 : 0;
}

TEST(WindowStateStoreTest, MissingStateUsesCallerDefaults) {
  TemporaryDirectory temporary;
  ASSERT_FALSE(temporary.path().empty());

  const WindowStateLoadResult loaded =
      LoadWindowState(temporary.path() / "window-state.json");
  EXPECT_TRUE(loaded);
  EXPECT_FALSE(loaded.found);
}

TEST(WindowStateStoreTest, RoundTripsGeometryAndPresentationState) {
  TemporaryDirectory temporary;
  const std::filesystem::path path =
      temporary.path() / "state/window-state.json";
  PersistedWindowState expected;
  expected.x = -1440;
  expected.y = 120;
  expected.width = 1440;
  expected.height = 900;
  expected.has_position = true;
  expected.fullscreen = true;
  expected.maximized = false;

  const Status stored = StoreWindowState(path, expected);
  ASSERT_TRUE(stored.ok()) << stored.message();
  EXPECT_EQ(FileMode(path), 0600);

  const WindowStateLoadResult loaded = LoadWindowState(path);
  ASSERT_TRUE(loaded) << loaded.status.message();
  ASSERT_TRUE(loaded.found);
  EXPECT_EQ(loaded.state.x, expected.x);
  EXPECT_EQ(loaded.state.y, expected.y);
  EXPECT_EQ(loaded.state.width, expected.width);
  EXPECT_EQ(loaded.state.height, expected.height);
  EXPECT_TRUE(loaded.state.has_position);
  EXPECT_TRUE(loaded.state.fullscreen);
  EXPECT_FALSE(loaded.state.maximized);
}

TEST(WindowStateStoreTest, RejectsMalformedAndOutOfRangeState) {
  TemporaryDirectory temporary;
  const std::filesystem::path path = temporary.path() / "window-state.json";
  {
    std::ofstream output(path);
    output << "{not-json";
  }
  EXPECT_FALSE(LoadWindowState(path));

  PersistedWindowState invalid;
  invalid.width = 100000;
  EXPECT_FALSE(StoreWindowState(path, invalid).ok());

  {
    std::ofstream output(path);
    output << R"({"schema_version":1,"fullscreen":false,)"
              R"("maximized":false,"windowed":{"has_position":false,)"
              R"("width":9223372036854775807,"height":720}})";
  }
  EXPECT_FALSE(LoadWindowState(path));
}

TEST(WindowStateStoreTest, RefusesSymlinkTargets) {
  TemporaryDirectory temporary;
  const std::filesystem::path outside = temporary.path() / "outside.json";
  const std::filesystem::path path = temporary.path() / "window-state.json";
  {
    std::ofstream output(outside);
    output << "preserve";
  }
  ASSERT_EQ(symlink(outside.c_str(), path.c_str()), 0);

  PersistedWindowState state;
  EXPECT_FALSE(StoreWindowState(path, state).ok());
  EXPECT_FALSE(LoadWindowState(path));

  std::ifstream input(outside);
  std::string contents;
  input >> contents;
  EXPECT_EQ(contents, "preserve");
}

}  // namespace
}  // namespace window
}  // namespace mocktail
