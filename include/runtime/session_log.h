#ifndef MOCKTAIL_RUNTIME_SESSION_LOG_H_
#define MOCKTAIL_RUNTIME_SESSION_LOG_H_

#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

#include "runtime/environment.h"
#include "runtime/runtime_paths.h"

namespace mocktail {
namespace runtime {

struct SessionBuildInformation {
  std::string version;
  std::string git_commit;
  std::string git_dirty;
  std::string build_type;
  std::string compiler;
  std::string target;
};

SessionBuildInformation CurrentSessionBuildInformation();

class SessionLog final {
 public:
  SessionLog() = default;
  ~SessionLog();

  SessionLog(const SessionLog&) = delete;
  SessionLog& operator=(const SessionLog&) = delete;
  SessionLog(SessionLog&& other) noexcept;
  SessionLog& operator=(SessionLog&& other) noexcept;

  static SessionLog Start(const Environment& environment,
                          const RuntimePaths& paths,
                          std::chrono::system_clock::time_point started_at =
                              std::chrono::system_clock::now());

  bool active() const { return active_; }
  bool attempted() const { return attempted_; }
  explicit operator bool() const { return active_; }

  const std::filesystem::path& path() const { return path_; }
  const std::filesystem::path& latest_path() const { return latest_path_; }
  const std::string& error() const { return error_; }
  const std::string& warning() const { return warning_; }

  std::string Header(const Environment& environment, const RuntimePaths& paths,
                     std::string_view graphics_backend) const;

 private:
  void Stop();

  bool attempted_ = false;
  bool active_ = false;
  int original_stdout_ = -1;
  int original_stderr_ = -1;
  int logger_process_ = -1;
  std::chrono::system_clock::time_point started_at_{};
  std::filesystem::path path_;
  std::filesystem::path latest_path_;
  std::string error_;
  std::string warning_;
};

}  // namespace runtime
}  // namespace mocktail

#endif  // MOCKTAIL_RUNTIME_SESSION_LOG_H_
