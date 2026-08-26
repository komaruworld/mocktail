// Standalone settings window for the documented config.yaml.
//
// Settings are read through the same loader the runtime uses and written back
// with the comment-preserving writer, so hand-written comments and untouched
// keys survive. Every change applies instantly and takes effect at the next
// launch, which the banner states.

#include <adwaita.h>

#include <cstdlib>
#include <string>
#include <vector>

#include "runtime/environment.h"
#include "runtime/frame_rate_policy.h"
#include "runtime/game_mode.h"
#include "runtime/performance_policy.h"
#include "runtime/runtime_config.h"
#include "runtime/runtime_config_file.h"
#include "runtime/runtime_config_writer.h"
#include "runtime/runtime_paths.h"

namespace {

namespace mr = mocktail::runtime;

struct Choice {
  const char* value;
  const char* label;
};

constexpr Choice kBackends[] = {
    {"direct-vulkan", "Vulkan (recommended)"},
    {"opengl", "OpenGL"},
    {"angle-vulkan", "ANGLE over Vulkan"},
    {"system", "System default"},
};

constexpr Choice kFrameRates[] = {
    {"display", "Match display"}, {"30", "30 FPS"},   {"60", "60 FPS"},
    {"120", "120 FPS"},           {"144", "144 FPS"}, {"240", "240 FPS"},
    {"unlimited", "Unlimited"},
};

constexpr Choice kVsync[] = {
    {"auto", "Automatic"},
    {"on", "On"},
    {"off", "Off"},
};

constexpr Choice kThemes[] = {
    {"system", "Follow system"},
    {"light", "Light"},
    {"dark", "Dark"},
};

constexpr Choice kPhysicsModes[] = {
    {"throughput", "Throughput (use every core)"},
    {"latency", "Latency (Roblox pool sizes)"},
    {"auto", "Automatic"},
};

constexpr Choice kGameModes[] = {
    {"auto", "When available"},
    {"on", "Always"},
    {"off", "Never"},
};

constexpr Choice kDevices[] = {
    {"pc-windows-11", "PC (Windows 11)"},
    {"mobile-pixel-7", "Mobile (Pixel 7)"},
    {"console-ps5", "Console (PS5)"},
};

struct Settings {
  AdwApplicationWindow* window = nullptr;
  AdwToastOverlay* toasts = nullptr;
  std::string config_path;
  // Set while widgets are populated, so programmatic updates do not write.
  bool loading = true;
};

void ShowToast(Settings* settings, const char* text) {
  adw_toast_overlay_add_toast(settings->toasts, adw_toast_new(text));
}

void Persist(Settings* settings, std::vector<std::string> path,
             const std::string& value) {
  if (settings->loading) {
    return;
  }
  mr::ConfigAssignment assignment;
  assignment.path = std::move(path);
  assignment.value = value;
  std::string error;
  if (!mr::WriteConfigAssignments(settings->config_path, {assignment},
                                  &error)) {
    const std::string message = "Could not save: " + error;
    ShowToast(settings, message.c_str());
    return;
  }
  ShowToast(settings, "Saved. Restart Mocktail to apply.");
}

// Each row owns the config path it writes to, freed with the widget.
struct RowTarget {
  Settings* settings;
  std::vector<std::string> path;
  const Choice* choices;
  unsigned choice_count;
};

void DestroyRowTarget(gpointer data, GClosure*) {
  delete static_cast<RowTarget*>(data);
}

RowTarget* MakeTarget(Settings* settings, std::vector<std::string> path,
                      const Choice* choices = nullptr,
                      unsigned choice_count = 0) {
  auto* target = new RowTarget{settings, std::move(path), choices,
                               choice_count};
  return target;
}

void OnComboChanged(AdwComboRow* row, GParamSpec*, gpointer data) {
  auto* target = static_cast<RowTarget*>(data);
  const guint selected = adw_combo_row_get_selected(row);
  if (selected >= target->choice_count) {
    return;
  }
  Persist(target->settings, target->path,
          mr::EncodeConfigScalar(target->choices[selected].value));
}

void OnSwitchChanged(AdwSwitchRow* row, GParamSpec*, gpointer data) {
  auto* target = static_cast<RowTarget*>(data);
  Persist(target->settings, target->path,
          adw_switch_row_get_active(row) ? "true" : "false");
}

void OnSpinChanged(AdwSpinRow* row, GParamSpec*, gpointer data) {
  auto* target = static_cast<RowTarget*>(data);
  Persist(target->settings, target->path,
          std::to_string(static_cast<long>(adw_spin_row_get_value(row))));
}

void OnEntryApplied(AdwEntryRow* row, gpointer data) {
  auto* target = static_cast<RowTarget*>(data);
  const char* text = gtk_editable_get_text(GTK_EDITABLE(row));
  Persist(target->settings, target->path,
          mr::EncodeConfigScalar(text != nullptr ? text : ""));
}

GtkStringList* BuildLabels(const Choice* choices, unsigned count) {
  GtkStringList* list = gtk_string_list_new(nullptr);
  for (unsigned index = 0; index < count; ++index) {
    gtk_string_list_append(list, choices[index].label);
  }
  return list;
}

guint IndexOf(const Choice* choices, unsigned count, const std::string& value) {
  for (unsigned index = 0; index < count; ++index) {
    if (value == choices[index].value) {
      return index;
    }
  }
  return 0;
}

AdwComboRow* AddCombo(AdwPreferencesGroup* group, Settings* settings,
                      const char* title, const char* subtitle,
                      const Choice* choices, unsigned count,
                      const std::string& current,
                      std::vector<std::string> path) {
  auto* row = ADW_COMBO_ROW(adw_combo_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  if (subtitle != nullptr) {
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
  }
  adw_combo_row_set_model(row, G_LIST_MODEL(BuildLabels(choices, count)));
  adw_combo_row_set_selected(row, IndexOf(choices, count, current));
  g_signal_connect_data(row, "notify::selected",
                        G_CALLBACK(OnComboChanged),
                        MakeTarget(settings, std::move(path), choices, count),
                        DestroyRowTarget, G_CONNECT_DEFAULT);
  adw_preferences_group_add(group, GTK_WIDGET(row));
  return row;
}

void AddSwitch(AdwPreferencesGroup* group, Settings* settings,
               const char* title, const char* subtitle, bool active,
               std::vector<std::string> path) {
  auto* row = ADW_SWITCH_ROW(adw_switch_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  if (subtitle != nullptr) {
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
  }
  adw_switch_row_set_active(row, active ? TRUE : FALSE);
  g_signal_connect_data(row, "notify::active", G_CALLBACK(OnSwitchChanged),
                        MakeTarget(settings, std::move(path)),
                        DestroyRowTarget, G_CONNECT_DEFAULT);
  adw_preferences_group_add(group, GTK_WIDGET(row));
}

void AddSpin(AdwPreferencesGroup* group, Settings* settings, const char* title,
             const char* subtitle, double value, double minimum, double maximum,
             double step, std::vector<std::string> path) {
  auto* row = ADW_SPIN_ROW(adw_spin_row_new_with_range(minimum, maximum, step));
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  if (subtitle != nullptr) {
    adw_action_row_set_subtitle(ADW_ACTION_ROW(row), subtitle);
  }
  adw_spin_row_set_value(row, value);
  g_signal_connect_data(row, "notify::value", G_CALLBACK(OnSpinChanged),
                        MakeTarget(settings, std::move(path)),
                        DestroyRowTarget, G_CONNECT_DEFAULT);
  adw_preferences_group_add(group, GTK_WIDGET(row));
}

void AddEntry(AdwPreferencesGroup* group, Settings* settings, const char* title,
              const char* subtitle, const std::string& value,
              std::vector<std::string> path) {
  auto* row = ADW_ENTRY_ROW(adw_entry_row_new());
  adw_preferences_row_set_title(ADW_PREFERENCES_ROW(row), title);
  adw_entry_row_set_show_apply_button(row, TRUE);
  gtk_editable_set_text(GTK_EDITABLE(row), value.c_str());
  if (subtitle != nullptr) {
    gtk_widget_set_tooltip_text(GTK_WIDGET(row), subtitle);
  }
  g_signal_connect_data(row, "apply", G_CALLBACK(OnEntryApplied),
                        MakeTarget(settings, std::move(path)),
                        DestroyRowTarget, G_CONNECT_DEFAULT);
  adw_preferences_group_add(group, GTK_WIDGET(row));
}

AdwPreferencesGroup* AddGroup(AdwPreferencesPage* page, const char* title,
                              const char* description) {
  auto* group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
  adw_preferences_group_set_title(group, title);
  if (description != nullptr) {
    adw_preferences_group_set_description(group, description);
  }
  adw_preferences_page_add(page, group);
  return group;
}

std::string FrameRateValue(const mr::RuntimeConfig& config) {
  const mr::FrameRatePolicy& policy = config.frame_rate();
  switch (policy.mode) {
    case mr::FrameRateLimitMode::kUnlimited:
      return "unlimited";
    case mr::FrameRateLimitMode::kFixed:
      return std::to_string(policy.fixed_fps);
    case mr::FrameRateLimitMode::kDisplay:
    case mr::FrameRateLimitMode::kInvalid:
      break;
  }
  return "display";
}

void BuildUi(Settings* settings, const mr::RuntimeConfig& config,
             bool device_is_mapping) {
  auto* page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());

  AdwPreferencesGroup* graphics =
      AddGroup(page, "Graphics", "Applied the next time Mocktail starts.");
  AddCombo(graphics, settings, "Renderer",
           "Use OpenGL only when Vulkan is unavailable.", kBackends,
           G_N_ELEMENTS(kBackends), config.graphics_backend_name(),
           {"graphics", "backend"});
  AddCombo(graphics, settings, "Frame rate limit", nullptr, kFrameRates,
           G_N_ELEMENTS(kFrameRates), FrameRateValue(config),
           {"graphics", "frame_rate_limit"});
  AddCombo(graphics, settings, "V-Sync", nullptr, kVsync,
           G_N_ELEMENTS(kVsync), config.vsync_mode(), {"graphics", "vsync"});

  AdwPreferencesGroup* appearance = AddGroup(page, "Appearance", nullptr);
  AddCombo(appearance, settings, "Theme", nullptr, kThemes,
           G_N_ELEMENTS(kThemes), config.theme_mode(),
           {"appearance", "theme"});

  AdwPreferencesGroup* device = AddGroup(
      page, "Device profile",
      "Reported to Roblox. The PC profile enables the desktop layout.");
  AdwComboRow* device_row =
      AddCombo(device, settings, "Preset", nullptr, kDevices,
               G_N_ELEMENTS(kDevices), config.device_profile().name,
               {"device"});
  if (device_is_mapping) {
    // A hand-written mapping cannot be replaced by a preset without losing it.
    gtk_widget_set_sensitive(GTK_WIDGET(device_row), FALSE);
    adw_action_row_set_subtitle(
        ADW_ACTION_ROW(device_row),
        "Managed by a custom device block in config.yaml.");
  }

  AdwPreferencesGroup* performance = AddGroup(page, "Performance", nullptr);
  AddSwitch(performance, settings, "Multithreaded rendering",
            "Size Roblox queues from every physical core.",
            config.performance().multithreaded_rendering,
            {"performance", "multithreaded_rendering"});
  AddCombo(performance, settings, "Physics workers", nullptr, kPhysicsModes,
           G_N_ELEMENTS(kPhysicsModes),
           std::string(mr::PhysicsWorkerModeName(
               config.performance().physics_worker_mode)),
           {"performance", "physics_worker_mode"});
  AddCombo(performance, settings, "Feral GameMode", nullptr, kGameModes,
           G_N_ELEMENTS(kGameModes),
           std::string(mr::GameModePolicyName(config.performance().game_mode)),
           {"performance", "gamemode"});
  AddSpin(performance, settings, "Memory limit",
          "MiB. 0 disables the cap.",
          static_cast<double>(config.performance().memory_limit_mb), 0, 131072,
          512, {"performance", "memory_limit_mb"});

  AdwPreferencesGroup* window = AddGroup(page, "Window", nullptr);
  AddSpin(window, settings, "Width", nullptr, config.window().width, 640, 15360,
          16, {"window", "width"});
  AddSpin(window, settings, "Height", nullptr, config.window().height, 480,
          8640, 16, {"window", "height"});
  AddEntry(window, settings, "Title", nullptr, config.window().title,
           {"window", "title"});

  AdwPreferencesGroup* audio = AddGroup(
      page, "Audio",
      "Use `default` to follow the host, or an exact device name from the "
      "startup log.");
  AddEntry(audio, settings, "Output device", nullptr,
           config.audio_output_device(), {"audio", "output_device"});
  AddEntry(audio, settings, "Input device",
           "Use `disabled` to deny Roblox the microphone.",
           config.audio_input_device(), {"audio", "input_device"});

  AdwPreferencesGroup* integrations = AddGroup(page, "Integrations", nullptr);
  AddSwitch(integrations, settings, "Discord Rich Presence",
            "Never signs in to Discord and never reads an account token.",
            config.discord_rpc().enabled,
            {"integrations", "discord_rpc", "enabled"});
  AddSwitch(integrations, settings, "System proxy",
            "Follow the host HTTP or SOCKS5 proxy.", config.use_system_proxy(),
            {"network", "use_system_proxy"});

  auto* banner = ADW_BANNER(adw_banner_new(
      "Changes are saved to config.yaml and apply the next time Mocktail "
      "starts."));
  adw_banner_set_revealed(banner, TRUE);

  auto* header = ADW_HEADER_BAR(adw_header_bar_new());
  auto* content = GTK_BOX(gtk_box_new(GTK_ORIENTATION_VERTICAL, 0));
  gtk_box_append(content, GTK_WIDGET(header));
  gtk_box_append(content, GTK_WIDGET(banner));
  gtk_box_append(content, GTK_WIDGET(page));
  gtk_widget_set_vexpand(GTK_WIDGET(page), TRUE);

  settings->toasts = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
  adw_toast_overlay_set_child(settings->toasts, GTK_WIDGET(content));
  adw_application_window_set_content(settings->window,
                                     GTK_WIDGET(settings->toasts));
}

// True when `device:` opens a mapping rather than holding a preset name.
bool DeviceIsMapping(const std::string& path) {
  std::string yaml;
  {
    FILE* file = std::fopen(path.c_str(), "re");
    if (file == nullptr) {
      return false;
    }
    char buffer[4096];
    std::size_t count = 0;
    while ((count = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
      yaml.append(buffer, count);
    }
    std::fclose(file);
  }
  std::size_t offset = 0;
  while (offset < yaml.size()) {
    const std::size_t end = yaml.find('\n', offset);
    const std::string line =
        yaml.substr(offset, end == std::string::npos ? std::string::npos
                                                     : end - offset);
    if (line.rfind("device:", 0) == 0) {
      return line.find_first_not_of(" \t", 7) == std::string::npos;
    }
    if (end == std::string::npos) {
      break;
    }
    offset = end + 1;
  }
  return false;
}

void OnActivate(AdwApplication* application, gpointer data) {
  auto* settings = static_cast<Settings*>(data);
  settings->loading = true;

  const mr::ProcessEnvironment environment;
  const mr::RuntimePaths paths = mr::RuntimePaths::FromEnvironment(environment);
  settings->config_path = paths.config_file().string();
  const mr::RuntimeConfigLoadResult loaded =
      mr::LoadRuntimeConfig(environment, paths.config_file());

  settings->window = ADW_APPLICATION_WINDOW(
      adw_application_window_new(GTK_APPLICATION(application)));
  gtk_window_set_title(GTK_WINDOW(settings->window), "Mocktail Settings");
  gtk_window_set_default_size(GTK_WINDOW(settings->window), 640, 760);

  if (!loaded) {
    auto* status = ADW_STATUS_PAGE(adw_status_page_new());
    adw_status_page_set_icon_name(status, "dialog-error-symbolic");
    adw_status_page_set_title(status, "Cannot read the configuration");
    adw_status_page_set_description(status, loaded.error.c_str());
    adw_application_window_set_content(settings->window, GTK_WIDGET(status));
  } else {
    BuildUi(settings, loaded.config, DeviceIsMapping(settings->config_path));
  }

  settings->loading = false;
  gtk_window_present(GTK_WINDOW(settings->window));
}

}  // namespace

int main(int argc, char** argv) {
  Settings settings;
  AdwApplication* application = adw_application_new(
      "space.bigrat.mocktail.Settings", G_APPLICATION_DEFAULT_FLAGS);
  g_signal_connect(application, "activate", G_CALLBACK(OnActivate), &settings);
  const int status = g_application_run(G_APPLICATION(application), argc, argv);
  g_object_unref(application);
  return status;
}
