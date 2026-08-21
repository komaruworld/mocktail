#include "runtime/roblox_text_input_jni_bridge.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <utility>

#include "jnivm/jnivm.h"
#include "runtime/roblox_window_input_runtime.h"
#include "window/window.h"

namespace mocktail {
namespace runtime {
namespace {

constexpr std::size_t kMaximumPendingTextCommands = 64;
constexpr std::size_t kMaximumPendingTextBytes = 4U * 1024U * 1024U;

void SecureClear(std::string* value) {
  if (value == nullptr) {
    return;
  }
  volatile char* bytes = value->empty() ? nullptr : value->data();
  for (std::size_t index = 0; index < value->size(); ++index) {
    bytes[index] = 0;
  }
  value->clear();
}

int RoundedCoordinate(float value) {
  if (!std::isfinite(value) || value <= 0.0F) {
    return 0;
  }
  const double rounded = std::round(static_cast<double>(value));
  return rounded > static_cast<double>(std::numeric_limits<int>::max())
             ? 0
             : static_cast<int>(rounded);
}

bool HasUsableGeometry(const jnivm::RobloxTextBoxInfo& info) {
  return RoundedCoordinate(info.width) > 0 &&
         RoundedCoordinate(info.height) > 0;
}

void PreserveGeometry(const jnivm::RobloxTextBoxInfo& source,
                      jnivm::RobloxTextBoxInfo* destination) {
  if (destination == nullptr) {
    return;
  }
  destination->x = source.x;
  destination->y = source.y;
  destination->width = source.width;
  destination->height = source.height;
}

jnivm::RobloxTextBoxInfo ToTextBoxInfo(
    const RobloxNativeTextBoxInfoSnapshot& snapshot) {
  jnivm::RobloxTextBoxInfo info;
  info.x = snapshot.x;
  info.y = snapshot.y;
  info.width = snapshot.width;
  info.height = snapshot.height;
  info.font_size = snapshot.font_size;
  info.multiline = snapshot.multiline;
  info.x_alignment = snapshot.x_alignment;
  info.y_alignment = snapshot.y_alignment;
  info.text_color = snapshot.text_color;
  info.font = snapshot.font;
  info.text_input_type = snapshot.text_input_type;
  info.return_key_type = snapshot.return_key_type;
  info.manual_focus_release = snapshot.manual_focus_release;
  info.text_wrapped = snapshot.text_wrapped;
  return info;
}

RobloxTextFocusProperties ToFocusProperties(
    const jnivm::RobloxTextBoxInfo& info) {
  RobloxTextFocusProperties properties;
  properties.manual_focus_release = info.manual_focus_release;
  properties.multiline = info.multiline;
  properties.area_x = RoundedCoordinate(info.x);
  properties.area_y = RoundedCoordinate(info.y);
  properties.area_width = RoundedCoordinate(info.width);
  properties.area_height = RoundedCoordinate(info.height);
  properties.font_size = info.font_size;
  properties.x_alignment = info.x_alignment;
  properties.y_alignment = info.y_alignment;
  properties.text_color = info.text_color;
  properties.font = info.font;
  properties.text_input_type = info.text_input_type;
  properties.return_key_type = info.return_key_type;
  properties.text_wrapped = info.text_wrapped;
  return properties;
}

window::TextInputArea ToTextInputArea(const jnivm::RobloxTextBoxInfo& info) {
  window::TextInputArea area;
  area.x = RoundedCoordinate(info.x);
  area.y = RoundedCoordinate(info.y);
  area.width = RoundedCoordinate(info.width);
  area.height = RoundedCoordinate(info.height);
  area.cursor = area.width;
  return area;
}

}  // namespace

class ProductionTextInputBackend final
    : public RobloxTextInputJniBridgeBackend {
 public:
  explicit ProductionTextInputBackend(
      std::shared_ptr<RobloxWindowInputRuntime> runtime)
      : runtime_(std::move(runtime)) {}

  Status BeginTextFocusSession(RobloxTextFocusSession session) override {
    return runtime_->BeginTextFocusSession(std::move(session));
  }
  Status EndTextFocusSession(int64_t textbox_handle, uint64_t generation,
                             bool notify_native) override {
    return runtime_->EndTextFocusSession(textbox_handle, generation,
                                         notify_native);
  }
  Status ReplaceFocusedTextFromEngine(uint64_t generation,
                                      std::string authoritative_utf8) override {
    return runtime_->ReplaceFocusedTextFromEngine(
        generation, std::move(authoritative_utf8));
  }
  Status QueryCurrentTextBoxInfo(
      RobloxNativeTextBoxInfoQueryResult* result) override {
    return runtime_->QueryCurrentTextBoxInfo(result);
  }
  Status UpdateTextFocusProperties(
      uint64_t generation,
      const RobloxTextFocusProperties& properties) override {
    return runtime_->UpdateTextFocusProperties(generation, properties);
  }
  bool RegisterMainThreadPump(window::PreTextInputPumpCallback callback,
                              void* context) override {
    return window::SetPreTextInputPumpCallback(callback, context);
  }
  void ClearMainThreadPump() override {
    window::ClearPreTextInputPumpCallback();
  }
  void SetTextInputOwnerEnabled(bool enabled) override {
    window::SetWindowTextInputOwnerEnabled(enabled);
  }
  bool RequestShowTextInput(uint64_t generation,
                            const window::TextInputArea& area,
                            const window::TextInputOptions& options) override {
    return window::RequestShowTextInput(generation, area, options);
  }
  bool RequestHideTextInput(uint64_t generation) override {
    return window::RequestHideTextInput(generation);
  }

 private:
  const std::shared_ptr<RobloxWindowInputRuntime> runtime_;
};

struct RobloxTextInputJniBridge::State {
  enum class CommandType { kShow, kHide, kReplaceText, kRefreshProperties };

  struct Command {
    CommandType type = CommandType::kHide;
    uint64_t generation = 0;
    int64_t textbox_handle = 0;
    std::string text;
    jnivm::RobloxTextBoxInfo info;

    void ClearSensitiveData() { SecureClear(&text); }
  };

  explicit State(std::shared_ptr<RobloxTextInputJniBridgeBackend> owner)
      : backend(std::move(owner)) {}

  ~State() { StopAccepting(); }

  void ClearCommandsLocked() {
    for (Command& command : commands) {
      command.ClearSensitiveData();
    }
    commands.clear();
    pending_text_bytes = 0;
  }

  void EnterTerminalLocked(const char* message) {
    if (terminal_error.empty()) {
      terminal_error =
          message != nullptr ? message : "Roblox text-input bridge failed";
    }
    accepting = false;
    desired_active = false;
    active_geometry = {};
    has_active_geometry = false;
    ClearCommandsLocked();
  }

  bool ReserveCommandLocked(std::size_t text_bytes) {
    if (!accepting) {
      return false;
    }
    if (text_bytes > kMaximumPendingTextBytes ||
        commands.size() >= kMaximumPendingTextCommands ||
        pending_text_bytes > kMaximumPendingTextBytes - text_bytes) {
      EnterTerminalLocked("Roblox text-input command queue overflow");
      return false;
    }
    pending_text_bytes += text_bytes;
    return true;
  }

  bool ReplacePendingTextBytesLocked(std::size_t old_size,
                                     std::size_t new_size) {
    if (old_size > pending_text_bytes) {
      EnterTerminalLocked("Roblox text-input queue accounting failed");
      return false;
    }
    const std::size_t retained_bytes = pending_text_bytes - old_size;
    if (new_size > kMaximumPendingTextBytes ||
        retained_bytes > kMaximumPendingTextBytes - new_size) {
      EnterTerminalLocked("Roblox text-input command queue overflow");
      return false;
    }
    pending_text_bytes = retained_bytes + new_size;
    return true;
  }

  // A duplicate show callback can arrive before SDL drains the first one.
  // Treat it as the latest snapshot of the same focus request instead of
  // constructing two editor generations. Any replace callbacks already
  // queued behind that show are older than the new authoritative snapshot.
  bool CoalescePendingShowLocked(
      const jnivm::RobloxTextInputShowRequest& request,
      const jnivm::RobloxTextBoxInfo& effective_info) {
    const auto pending_show =
        std::find_if(commands.begin(), commands.end(),
                     [this, &request](const Command& item) {
                       return item.type == CommandType::kShow &&
                              item.generation == active_generation &&
                              item.textbox_handle == request.text_box;
                     });
    if (pending_show == commands.end()) {
      return false;
    }

    std::size_t superseded_bytes = pending_show->text.size();
    for (const Command& item : commands) {
      if (item.type == CommandType::kReplaceText &&
          item.generation == active_generation) {
        superseded_bytes += item.text.size();
      }
    }
    if (!ReplacePendingTextBytesLocked(superseded_bytes, request.text.size())) {
      return true;
    }

    commands.erase(
        std::remove_if(commands.begin(), commands.end(),
                       [this](Command& item) {
                         if (item.type != CommandType::kReplaceText ||
                             item.generation != active_generation) {
                           return false;
                         }
                         item.ClearSensitiveData();
                         return true;
                       }),
        commands.end());
    const auto updated_show =
        std::find_if(commands.begin(), commands.end(),
                     [this, &request](const Command& item) {
                       return item.type == CommandType::kShow &&
                              item.generation == active_generation &&
                              item.textbox_handle == request.text_box;
                     });
    if (updated_show == commands.end()) {
      EnterTerminalLocked("Roblox text-input pending show was lost");
      return true;
    }
    updated_show->ClearSensitiveData();
    updated_show->text = request.text;
    updated_show->info = effective_info;
    active_info = effective_info;
    return true;
  }

  void Show(const jnivm::RobloxTextInputShowRequest& request) {
    Command command;
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (!accepting) {
        return;
      }

      jnivm::RobloxTextBoxInfo effective_info = request.info;
      const bool refreshes_active_focus =
          desired_active && active_generation != 0;
      const bool same_handle = active_handle == request.text_box;
      const bool can_reuse_active_geometry =
          refreshes_active_focus && has_active_geometry &&
          (same_handle || !request.show_native_input);
      bool inherited_geometry = false;
      if (!HasUsableGeometry(effective_info) && can_reuse_active_geometry) {
        // Roblox 2.725 reports one logical focus through both
        // NativeGLJavaInterface and EngineJavaCallback2. A non-native refresh
        // changes the engine handle but retains the shared RbxKeyboard view.
        PreserveGeometry(active_geometry, &effective_info);
        inherited_geometry = true;
      } else if (!HasUsableGeometry(effective_info) &&
                 request.show_native_input && !same_handle) {
        // A new native editor with no rectangle must not borrow the previous
        // TextBox position. A later property callback may supply real bounds.
        active_geometry = {};
        has_active_geometry = false;
      }
      if (HasUsableGeometry(effective_info)) {
        active_geometry = effective_info;
        has_active_geometry = true;
      }
      std::fprintf(stderr,
                   "  [input] TextBox focus snapshot native=%d "
                   "handle_changed=%d geometry=%dx%d inherited=%d\n",
                   request.show_native_input ? 1 : 0,
                   refreshes_active_focus && !same_handle ? 1 : 0,
                   RoundedCoordinate(effective_info.width),
                   RoundedCoordinate(effective_info.height),
                   inherited_geometry ? 1 : 0);
      if (refreshes_active_focus && same_handle &&
          CoalescePendingShowLocked(request, effective_info)) {
        return;
      }
      if (!ReserveCommandLocked(request.text.size())) {
        return;
      }
      ++generation;
      if (generation == 0) {
        EnterTerminalLocked("Roblox text-input generation overflow");
        return;
      }
      active_generation = generation;
      active_handle = request.text_box;
      desired_active = true;
      command.type = CommandType::kShow;
      command.generation = generation;
      command.textbox_handle = request.text_box;
      command.text = request.text;
      command.info = effective_info;
      commands.push_back(std::move(command));
      active_info = effective_info;
    }
  }

  void Hide() {
    std::lock_guard<std::mutex> lock(mutex);
    if (!accepting || !desired_active || active_generation == 0) {
      return;
    }
    if (!ReserveCommandLocked(0)) {
      return;
    }
    Command command;
    command.type = CommandType::kHide;
    command.generation = active_generation;
    command.textbox_handle = active_handle;
    commands.push_back(std::move(command));
    desired_active = false;
    active_info = {};
    active_geometry = {};
    has_active_geometry = false;
  }

  void ReplaceText(const std::string& text) {
    std::lock_guard<std::mutex> lock(mutex);
    if (!accepting || !desired_active || active_generation == 0) {
      return;
    }
    const auto pending_replace = std::find_if(
        commands.rbegin(), commands.rend(), [this](const Command& item) {
          return item.type == CommandType::kReplaceText &&
                 item.generation == active_generation;
        });
    if (pending_replace != commands.rend()) {
      if (!ReplacePendingTextBytesLocked(pending_replace->text.size(),
                                         text.size())) {
        return;
      }
      pending_replace->ClearSensitiveData();
      pending_replace->text = text;
      return;
    }
    if (!ReserveCommandLocked(text.size())) {
      return;
    }
    Command command;
    command.type = CommandType::kReplaceText;
    command.generation = active_generation;
    command.textbox_handle = active_handle;
    command.text = text;
    commands.push_back(std::move(command));
  }

  void RefreshProperties() {
    std::lock_guard<std::mutex> lock(mutex);
    if (!accepting || !desired_active || active_generation == 0 ||
        active_handle == 0) {
      return;
    }
    const auto duplicate = std::find_if(
        commands.begin(), commands.end(), [this](const Command& item) {
          return item.type == CommandType::kRefreshProperties &&
                 item.generation == active_generation &&
                 item.textbox_handle == active_handle;
        });
    if (duplicate != commands.end() || !ReserveCommandLocked(0)) {
      return;
    }
    Command command;
    command.type = CommandType::kRefreshProperties;
    command.generation = active_generation;
    command.textbox_handle = active_handle;
    commands.push_back(std::move(command));
  }

  bool IsDesiredFocusCurrent(uint64_t requested_generation,
                             int64_t requested_handle) const {
    std::lock_guard<std::mutex> lock(mutex);
    return accepting && desired_active &&
           active_generation == requested_generation &&
           active_handle == requested_handle;
  }

  bool CommitPropertySnapshotIfCurrent(
      uint64_t requested_generation, int64_t requested_handle,
      const RobloxNativeTextBoxInfoSnapshot& snapshot,
      jnivm::RobloxTextBoxInfo* effective_info) {
    if (effective_info == nullptr) {
      return false;
    }
    std::lock_guard<std::mutex> lock(mutex);
    if (!accepting || !desired_active ||
        active_generation != requested_generation ||
        active_handle != requested_handle) {
      return false;
    }
    jnivm::RobloxTextBoxInfo candidate = ToTextBoxInfo(snapshot);
    if (!HasUsableGeometry(candidate)) {
      if (!has_active_geometry) {
        return false;
      }
      PreserveGeometry(active_geometry, &candidate);
    } else {
      active_geometry = candidate;
      has_active_geometry = true;
    }
    active_info = candidate;
    *effective_info = candidate;
    return true;
  }

  bool Drain() {
    std::deque<Command> pending;
    bool terminal = false;
    {
      std::lock_guard<std::mutex> lock(mutex);
      terminal = !terminal_error.empty();
      pending.swap(commands);
      pending_text_bytes = 0;
    }

    if (terminal) {
      for (Command& command : pending) {
        command.ClearSensitiveData();
      }
      return CompleteTerminalOnMainThread("queue");
    }

    bool success = true;
    for (Command& command : pending) {
      if (!CompletionStatus().ok()) {
        for (Command& remaining : pending) {
          remaining.ClearSensitiveData();
        }
        return CompleteTerminalOnMainThread("concurrent-callback");
      }
      Status status = Status::Ok();
      const char* failure_message = "Roblox text-input command failed";
      const char* failure_stage = "command";
      if (command.type == CommandType::kShow) {
        failure_message = "Roblox TextBox focus session failed";
        failure_stage = "begin-focus";
        const window::TextInputArea area = ToTextInputArea(command.info);
        if (!area.valid() && !logged_missing_overlay_geometry) {
          logged_missing_overlay_geometry = true;
          std::fprintf(stderr,
                       "  [input] Roblox NativeTextBoxInfo geometry is "
                       "unavailable generation=%llu type=%d width=%d "
                       "height=%d; native text input remains authoritative\n",
                       static_cast<unsigned long long>(command.generation),
                       command.info.text_input_type, area.width, area.height);
        }
        RobloxTextFocusSession session;
        session.textbox_handle = command.textbox_handle;
        session.generation = command.generation;
        session.initial_text = std::move(command.text);
        session.manual_focus_release = command.info.manual_focus_release;
        session.multiline = command.info.multiline;
        session.area_x = area.x;
        session.area_y = area.y;
        session.area_width = area.width;
        session.area_height = area.height;
        session.font_size = command.info.font_size;
        session.x_alignment = command.info.x_alignment;
        session.y_alignment = command.info.y_alignment;
        session.text_color = command.info.text_color;
        session.text_input_type = command.info.text_input_type;
        status = backend->BeginTextFocusSession(std::move(session));
        const window::TextInputOptions options{command.info.text_input_type,
                                               command.info.multiline};
        if (status.ok()) {
          applied_active = true;
          applied_generation = command.generation;
          applied_handle = command.textbox_handle;
          failure_stage = "request-show";
          if (!backend->RequestShowTextInput(command.generation, area,
                                             options)) {
            failure_message = "SDL text-input show request was rejected";
            status = Status::Error(StatusCode::kPlatformError, failure_message);
            (void)backend->EndTextFocusSession(command.textbox_handle,
                                               command.generation, true);
            (void)backend->RequestHideTextInput(command.generation);
            applied_active = false;
            applied_generation = 0;
            applied_handle = 0;
          } else {
            std::fprintf(stderr,
                         "  [input] real Roblox TextBox focus captured "
                         "generation=%llu type=%d multiline=%d\n",
                         static_cast<unsigned long long>(command.generation),
                         command.info.text_input_type,
                         command.info.multiline ? 1 : 0);
          }
        }
      } else if (command.type == CommandType::kHide) {
        failure_message = "Roblox TextBox focus release failed";
        failure_stage = "end-focus";
        status = backend->EndTextFocusSession(command.textbox_handle,
                                              command.generation, false);
        if (status.ok()) {
          applied_active = false;
          applied_generation = 0;
          applied_handle = 0;
        }
        if (status.ok() && !backend->RequestHideTextInput(command.generation)) {
          failure_stage = "request-hide";
          failure_message = "SDL text-input hide request was rejected";
          status = Status::Error(StatusCode::kPlatformError, failure_message);
        }
      } else if (command.type == CommandType::kReplaceText) {
        failure_message = "Roblox TextBox authoritative update failed";
        failure_stage = "replace";
        status = backend->ReplaceFocusedTextFromEngine(command.generation,
                                                       std::move(command.text));
      } else if (applied_active && applied_generation == command.generation &&
                 applied_handle == command.textbox_handle &&
                 IsDesiredFocusCurrent(command.generation,
                                       command.textbox_handle)) {
        failure_message = "Roblox TextBox property refresh failed";
        failure_stage = "query-properties";
        RobloxNativeTextBoxInfoQueryResult query;
        status = backend->QueryCurrentTextBoxInfo(&query);
        if (status.ok() && query.available) {
          jnivm::RobloxTextBoxInfo effective_info;
          if (CommitPropertySnapshotIfCurrent(command.generation,
                                              command.textbox_handle,
                                              query.info, &effective_info)) {
            failure_stage = "update-properties";
            status = backend->UpdateTextFocusProperties(
                command.generation, ToFocusProperties(effective_info));
            const window::TextInputArea area = ToTextInputArea(effective_info);
            const window::TextInputOptions options{
                effective_info.text_input_type, effective_info.multiline};
            if (status.ok() && !backend->RequestShowTextInput(
                                   command.generation, area, options)) {
              failure_stage = "request-property-show";
              failure_message = "SDL text-input property refresh was rejected";
              status =
                  Status::Error(StatusCode::kPlatformError, failure_message);
            } else if (status.ok() && !logged_property_refresh) {
              logged_property_refresh = true;
              std::fprintf(stderr,
                           "  [input] native TextBox properties refreshed "
                           "generation=%llu geometry=%dx%d\n",
                           static_cast<unsigned long long>(command.generation),
                           RoundedCoordinate(effective_info.width),
                           RoundedCoordinate(effective_info.height));
            }
          }
        }
      }
      command.ClearSensitiveData();
      if (status.ok()) {
        const Status terminal_status = CompletionStatus();
        if (!terminal_status.ok()) {
          status = terminal_status;
        }
      }
      if (!status.ok()) {
        for (Command& remaining : pending) {
          remaining.ClearSensitiveData();
        }
        {
          std::lock_guard<std::mutex> lock(mutex);
          EnterTerminalLocked(failure_message);
        }
        (void)CompleteTerminalOnMainThread(failure_stage);
        success = false;
        break;
      }
    }
    return success;
  }

  // Returns false exactly once so the event owner records the transition.
  // The terminal Status remains latched for readiness/shutdown, but later
  // pumps become successful no-ops instead of producing one line per frame.
  bool CompleteTerminalOnMainThread(const char* stage) {
    if (terminal_cleanup_completed) {
      return true;
    }
    const Status status = CompletionStatus();
    FailClosedOnMainThread();
    terminal_cleanup_completed = true;
    std::fprintf(stderr,
                 "  [input] typed TextBox bridge failed closed stage=%s "
                 "code=%d reason=%s\n",
                 stage != nullptr ? stage : "unknown",
                 static_cast<int>(status.code()), status.message().c_str());
    return false;
  }

  void FailClosedOnMainThread() {
    if (applied_active) {
      (void)backend->EndTextFocusSession(applied_handle, applied_generation,
                                         true);
      (void)backend->RequestHideTextInput(applied_generation);
      applied_active = false;
      applied_generation = 0;
      applied_handle = 0;
    }
    backend->SetTextInputOwnerEnabled(false);
  }

  void StopAccepting() {
    std::lock_guard<std::mutex> lock(mutex);
    accepting = false;
    desired_active = false;
    active_generation = 0;
    active_handle = 0;
    active_info = {};
    active_geometry = {};
    has_active_geometry = false;
    ClearCommandsLocked();
  }

  Status CompletionStatus() const {
    std::lock_guard<std::mutex> lock(mutex);
    return terminal_error.empty()
               ? Status::Ok()
               : Status::Error(StatusCode::kPlatformError, terminal_error);
  }

  static void ShowCallback(void* context,
                           const jnivm::RobloxTextInputShowRequest& request) {
    static_cast<State*>(context)->Show(request);
  }
  static void HideCallback(void* context) {
    static_cast<State*>(context)->Hide();
  }
  static void ReplaceTextCallback(void* context, const std::string& text) {
    static_cast<State*>(context)->ReplaceText(text);
  }
  static void PropertiesChangedCallback(void* context) {
    static_cast<State*>(context)->RefreshProperties();
  }
  static void ShutdownCallback(void* context) {
    static_cast<State*>(context)->StopAccepting();
  }
  static bool MainThreadPumpCallback(void* context) {
    return static_cast<State*>(context)->Drain();
  }

  const std::shared_ptr<RobloxTextInputJniBridgeBackend> backend;
  mutable std::mutex mutex;
  std::deque<Command> commands;
  uint64_t generation = 0;
  uint64_t active_generation = 0;
  int64_t active_handle = 0;
  jnivm::RobloxTextBoxInfo active_info;
  jnivm::RobloxTextBoxInfo active_geometry;
  bool has_active_geometry = false;
  bool desired_active = false;
  std::size_t pending_text_bytes = 0;
  bool accepting = true;
  bool terminal_cleanup_completed = false;
  bool logged_missing_overlay_geometry = false;
  bool logged_property_refresh = false;
  bool applied_active = false;
  uint64_t applied_generation = 0;
  int64_t applied_handle = 0;
  std::string terminal_error;
};

RobloxTextInputJniBridge::RobloxTextInputJniBridge(jnivm::VM* vm,
                                                   std::shared_ptr<State> state)
    : vm_(vm), state_(std::move(state)) {}

RobloxTextInputJniBridge::~RobloxTextInputJniBridge() { (void)Shutdown(); }

Status RobloxTextInputJniBridge::Create(
    jnivm::VM* vm, std::shared_ptr<RobloxWindowInputRuntime> input_runtime,
    std::unique_ptr<RobloxTextInputJniBridge>* bridge) {
  if (vm == nullptr || input_runtime == nullptr || bridge == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "text-input bridge requires VM, runtime and output");
  }
  auto backend = std::shared_ptr<RobloxTextInputJniBridgeBackend>(
      new (std::nothrow) ProductionTextInputBackend(std::move(input_runtime)));
  if (backend == nullptr) {
    return Status::Error(StatusCode::kUnavailable,
                         "unable to allocate text-input production backend");
  }
  return CreateForTesting(vm, std::move(backend), bridge);
}

Status RobloxTextInputJniBridge::CreateForTesting(
    jnivm::VM* vm, std::shared_ptr<RobloxTextInputJniBridgeBackend> backend,
    std::unique_ptr<RobloxTextInputJniBridge>* bridge) {
  if (vm == nullptr || backend == nullptr || bridge == nullptr) {
    return Status::Error(StatusCode::kInvalidArgument,
                         "text-input bridge requires VM, backend and output");
  }
  bridge->reset();
  auto state = std::shared_ptr<State>(new (std::nothrow) State(backend));
  if (state == nullptr) {
    return Status::Error(StatusCode::kUnavailable,
                         "unable to allocate text-input bridge state");
  }
  auto implementation = std::unique_ptr<RobloxTextInputJniBridge>(
      new (std::nothrow) RobloxTextInputJniBridge(vm, state));
  if (implementation == nullptr) {
    return Status::Error(StatusCode::kUnavailable,
                         "unable to allocate text-input bridge");
  }

  // Reserve the exclusive main-thread callback before mutating either the VM
  // binding or the global SDL text-input owner. A failed registration must not
  // clear or disable an existing owner's state during local RAII destruction.
  if (!backend->RegisterMainThreadPump(&State::MainThreadPumpCallback,
                                       state.get())) {
    return Status::Error(StatusCode::kFailedPrecondition,
                         "window text-input command pump is unavailable");
  }

  jnivm::RobloxTextInputCallbacks callbacks;
  callbacks.show = &State::ShowCallback;
  callbacks.hide = &State::HideCallback;
  callbacks.replace_text = &State::ReplaceTextCallback;
  callbacks.properties_changed = &State::PropertiesChangedCallback;
  callbacks.shutdown = &State::ShutdownCallback;
  vm->SetRobloxTextInputCallbacks(state, callbacks);
  backend->SetTextInputOwnerEnabled(true);
  implementation->installed_ = true;
  *bridge = std::move(implementation);
  return Status::Ok();
}

Status RobloxTextInputJniBridge::Shutdown() {
  if (!installed_) {
    return state_ == nullptr ? Status::Ok() : state_->CompletionStatus();
  }
  installed_ = false;
  state_->backend->ClearMainThreadPump();
  if (vm_ != nullptr) {
    vm_->ClearRobloxTextInputCallbacks();
  }
  if (state_ != nullptr) {
    state_->StopAccepting();
    state_->FailClosedOnMainThread();
  }
  return state_ == nullptr ? Status::Ok() : state_->CompletionStatus();
}

}  // namespace runtime
}  // namespace mocktail
