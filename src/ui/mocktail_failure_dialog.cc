#include <adwaita.h>
#include <glib-unix.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <string>
#include <string_view>

namespace {

constexpr char kMessagePacket = 'M';
constexpr char kFailurePacket = 'F';
constexpr char kSuccessPacket = 'S';
constexpr char kProgressPacket = 'P';
constexpr std::size_t kMaximumPacketBytes = 4096;
constexpr std::string_view kDefaultMessage =
    "Mocktail closed unexpectedly because of an internal error.";
constexpr std::string_view kDefaultProgressMessage = "Installing Roblox...";
// Keep AdwAlertDialog semantics while preventing host GTK themes such as
// Breeze from replacing the compact dark crash-card appearance.
constexpr char kDialogStyle[] = R"css(
dialog.alert,
window.dialog-window.alert {
  background-color: #38393e;
  color: #ffffff;
  border-radius: 18px;
  border: none;
  outline: none;
  box-shadow: none;
}

.alert .message-area {
  padding-top: 32px;
  padding-bottom: 9px;
  border-spacing: 24px;
}

.alert .message-area.has-heading.has-body {
  border-spacing: 10px;
}

.alert .message-area > .heading-bin,
.alert .message-area > .body,
.alert .message-area > .child {
  padding-left: 24px;
  padding-right: 24px;
}

.alert .heading-bin > label {
  color: #ffffff;
  font-size: 18px;
  font-weight: 700;
}

.alert .body {
  color: rgba(255, 255, 255, 0.92);
  font-size: 14px;
}

.alert .response-area {
  padding: 24px;
  padding-top: 12px;
  border-spacing: 12px;
}

.alert .response-area > button {
  min-height: 24px;
  min-width: 60px;
  padding: 10px 20px;
  border: none;
  border-radius: 12px;
  color: #ffffff;
  background: #515257;
  box-shadow: none;
}

.alert .response-area > button:hover {
  background: #5c5d62;
}

.alert .response-area > button:active {
  background: #48494e;
}
)css";

constexpr char kProgressStyle[] = R"css(
window.mocktail-update-progress {
  background-color: #202125;
  color: #f7f7f8;
}

window.mocktail-update-progress headerbar {
  background: transparent;
  border: none;
  box-shadow: none;
}

.mocktail-update-progress-content {
  border-spacing: 34px;
}

.mocktail-update-progress-label {
  color: #f7f7f8;
  font-size: 26px;
  font-weight: 700;
}

.mocktail-update-progress-spinner {
  min-width: 64px;
  min-height: 64px;
  -gtk-icon-size: 64px;
  color: #f7f7f8;
}
)css";

struct ProgressState {
  GMainLoop* loop = nullptr;
  GtkWindow* window = nullptr;
  GtkLabel* label = nullptr;
  guint source_id = 0;
};

enum class UiStyle {
  kDialog,
  kProgress,
};

std::string ValidMessage(std::string_view message) {
  if (message.empty() ||
      !g_utf8_validate(message.data(), static_cast<gssize>(message.size()),
                       nullptr)) {
    return std::string(kDefaultMessage);
  }
  return std::string(message);
}

void InstallDialogStyle(GdkDisplay* display) {
  GtkCssProvider* provider = gtk_css_provider_new();
  gtk_css_provider_load_from_string(provider, kDialogStyle);
  gtk_style_context_add_provider_for_display(
      display, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_USER);
  g_object_unref(provider);
}

void InstallProgressStyle(GdkDisplay* display) {
  GtkCssProvider* provider = gtk_css_provider_new();
  gtk_css_provider_load_from_string(provider, kProgressStyle);
  gtk_style_context_add_provider_for_display(
      display, GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_USER);
  g_object_unref(provider);
}

bool InitializeUi(UiStyle style) {
  g_set_application_name("Mocktail");
  if (!gtk_init_check()) {
    return false;
  }
  // Libadwaita owns the color scheme below; clear the unsupported legacy
  // preference before initialization so KDE settings do not emit a warning.
  GtkSettings* gtk_settings = gtk_settings_get_default();
  if (gtk_settings != nullptr) {
    g_object_set(gtk_settings, "gtk-application-prefer-dark-theme", FALSE,
                 nullptr);
  }
  adw_init();
  GdkDisplay* display = gdk_display_get_default();
  if (display == nullptr) {
    return false;
  }
  gtk_window_set_default_icon_name("space.bigrat.mocktail");
  adw_style_manager_set_color_scheme(adw_style_manager_get_default(),
                                     ADW_COLOR_SCHEME_FORCE_DARK);
  adw_style_manager_set_color_scheme(adw_style_manager_get_for_display(display),
                                     ADW_COLOR_SCHEME_FORCE_DARK);
  if (style == UiStyle::kDialog) {
    InstallDialogStyle(display);
  } else {
    InstallProgressStyle(display);
  }
  return true;
}

void OnDialogClosed(AdwDialog*, gpointer user_data) {
  g_main_loop_quit(static_cast<GMainLoop*>(user_data));
}

int ShowDialog(std::string_view requested_message,
               const char* heading = "Crash",
               const char* response_label = "Close") {
  if (!InitializeUi(UiStyle::kDialog)) {
    return EXIT_FAILURE;
  }

  const std::string message = ValidMessage(requested_message);
  GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
  AdwDialog* dialog = adw_alert_dialog_new(heading, message.c_str());
  g_object_ref_sink(dialog);
  adw_dialog_set_content_width(dialog, 270);
  AdwAlertDialog* alert = ADW_ALERT_DIALOG(dialog);
  adw_alert_dialog_add_response(alert, "close", response_label);
  adw_alert_dialog_set_close_response(alert, "close");
  adw_alert_dialog_set_default_response(alert, "close");
  g_signal_connect(dialog, "closed", G_CALLBACK(OnDialogClosed), loop);
  adw_dialog_present(dialog, nullptr);
  g_main_loop_run(loop);
  g_object_unref(dialog);
  g_main_loop_unref(loop);
  return EXIT_SUCCESS;
}

std::string ValidProgressMessage(std::string_view message) {
  if (message.empty() ||
      !g_utf8_validate(message.data(), static_cast<gssize>(message.size()),
                       nullptr)) {
    return std::string(kDefaultProgressMessage);
  }
  return std::string(message);
}

gboolean OnProgressWindowClose(GtkWindow*, gpointer user_data) {
  auto* state = static_cast<ProgressState*>(user_data);
  g_main_loop_quit(state->loop);
  return FALSE;
}

void PresentProgressWindow(ProgressState* state, std::string_view message) {
  GtkWidget* window_widget = adw_window_new();
  state->window = GTK_WINDOW(g_object_ref_sink(window_widget));
  gtk_widget_add_css_class(window_widget, "mocktail-update-progress");
  gtk_window_set_title(state->window, "Mocktail");
  gtk_window_set_default_size(state->window, 470, 640);

  GtkWidget* toolbar_widget = adw_toolbar_view_new();
  auto* toolbar = ADW_TOOLBAR_VIEW(toolbar_widget);
  GtkWidget* header_widget = adw_header_bar_new();
  adw_header_bar_set_show_title(ADW_HEADER_BAR(header_widget), FALSE);
  adw_toolbar_view_add_top_bar(toolbar, header_widget);

  GtkWidget* content = gtk_box_new(GTK_ORIENTATION_VERTICAL, 34);
  gtk_widget_add_css_class(content, "mocktail-update-progress-content");
  gtk_widget_set_halign(content, GTK_ALIGN_CENTER);
  gtk_widget_set_valign(content, GTK_ALIGN_CENTER);
  gtk_widget_set_hexpand(content, TRUE);
  gtk_widget_set_vexpand(content, TRUE);

  const std::string valid_message = ValidProgressMessage(message);
  GtkWidget* label_widget = gtk_label_new(valid_message.c_str());
  state->label = GTK_LABEL(label_widget);
  gtk_widget_add_css_class(label_widget, "mocktail-update-progress-label");
  gtk_label_set_justify(state->label, GTK_JUSTIFY_CENTER);
  gtk_label_set_wrap(state->label, TRUE);
  gtk_label_set_max_width_chars(state->label, 32);
  gtk_box_append(GTK_BOX(content), label_widget);

  GtkWidget* spinner = adw_spinner_new();
  gtk_widget_add_css_class(spinner, "mocktail-update-progress-spinner");
  gtk_widget_set_halign(spinner, GTK_ALIGN_CENTER);
  gtk_widget_set_size_request(spinner, 64, 64);
  gtk_box_append(GTK_BOX(content), spinner);

  adw_toolbar_view_set_content(toolbar, content);
  adw_window_set_content(ADW_WINDOW(window_widget), toolbar_widget);
  g_signal_connect(state->window, "close-request",
                   G_CALLBACK(OnProgressWindowClose), state);
  gtk_window_present(state->window);
}

gboolean OnProgressPacket(gint descriptor, GIOCondition condition,
                          gpointer user_data) {
  auto* state = static_cast<ProgressState*>(user_data);
  if ((condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) != 0) {
    state->source_id = 0;
    g_main_loop_quit(state->loop);
    return G_SOURCE_REMOVE;
  }

  char packet[kMaximumPacketBytes];
  const ssize_t received = recv(descriptor, packet, sizeof(packet), 0);
  if (received < 0 && (errno == EINTR || errno == EAGAIN)) {
    return G_SOURCE_CONTINUE;
  }
  if (received <= 0 || packet[0] == kSuccessPacket ||
      packet[0] == kFailurePacket) {
    state->source_id = 0;
    g_main_loop_quit(state->loop);
    return G_SOURCE_REMOVE;
  }
  if (packet[0] != kProgressPacket) {
    return G_SOURCE_CONTINUE;
  }

  const std::string message = ValidProgressMessage(
      std::string_view(packet + 1, static_cast<std::size_t>(received - 1)));
  if (state->window == nullptr) {
    PresentProgressWindow(state, message);
  } else {
    gtk_label_set_text(state->label, message.c_str());
  }
  return G_SOURCE_CONTINUE;
}

int MonitorProgress() {
  if (!InitializeUi(UiStyle::kProgress)) {
    return EXIT_FAILURE;
  }
  ProgressState state;
  state.loop = g_main_loop_new(nullptr, FALSE);
  state.source_id = g_unix_fd_add(
      STDIN_FILENO,
      static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL),
      OnProgressPacket, &state);
  if (state.source_id == 0) {
    g_main_loop_unref(state.loop);
    return EXIT_FAILURE;
  }
  g_main_loop_run(state.loop);
  if (state.source_id != 0) {
    g_source_remove(state.source_id);
  }
  if (state.window != nullptr) {
    gtk_window_destroy(state.window);
    g_object_unref(state.window);
  }
  g_main_loop_unref(state.loop);
  return EXIT_SUCCESS;
}

int MonitorParent() {
  std::string message(kDefaultMessage);
  char packet[kMaximumPacketBytes];
  while (true) {
    const ssize_t received = recv(STDIN_FILENO, packet, sizeof(packet), 0);
    if (received < 0 && errno == EINTR) {
      continue;
    }
    if (received <= 0) {
      return ShowDialog(message);
    }
    const std::string_view payload(packet + 1,
                                   static_cast<std::size_t>(received - 1));
    switch (packet[0]) {
      case kMessagePacket:
        message = ValidMessage(payload);
        break;
      case kFailurePacket:
        return ShowDialog(message);
      case kSuccessPacket:
        return EXIT_SUCCESS;
      default:
        break;
    }
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc == 2 && std::string_view(argv[1]) == "--monitor") {
    return MonitorParent();
  }
  if (argc == 2 && std::string_view(argv[1]) == "--progress-monitor") {
    return MonitorProgress();
  }
  if (argc == 3 && std::string_view(argv[1]) == "--message") {
    return ShowDialog(argv[2]);
  }
  if (argc == 3 && std::string_view(argv[1]) == "--warning") {
    return ShowDialog(argv[2], "Signed out", "Continue");
  }
  return EXIT_FAILURE;
}
