// cc -O2 -o mocktail_webview_host mocktail_webview_host.c $(pkg-config --cflags --libs gtk4 webkitgtk-6.0)

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <gtk/gtk.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <webkit/webkit.h>

#define PORT 2137
#define MAX_PACKET (64u * 1024u + 12u)
#define VERSION "2.725.1142"

enum {
  OP_LOAD_URL = 1, OP_SET_TITLE = 2, OP_SET_VISIBLE = 3, OP_EVAL_JS = 4,
  OP_CLOSE = 5, OP_SET_BACK_DISABLED = 6, OP_SHOW_DOMAIN_TITLE = 7,
  OP_SET_COOKIE = 8, OP_CLEAR_COOKIE = 9, OP_RETAIN_COOKIE = 10
};

enum { EV_EXECUTE_ROBLOX = 1, EV_ROBLOX_WK_HYBRID = 2, EV_READY = 3, EV_COOKIE = 4 };

static const char kBridge[] =
  "(() => { 'use strict';"
  " const h = window.webkit && window.webkit.messageHandlers;"
  " const handler = h && h.executeRoblox; if (!handler) return;"
  " const bridge = {};"
  " Object.defineProperty(bridge, 'executeRoblox', {"
  "   value: (q) => handler.postMessage(JSON.parse(q)),"
  "   enumerable: true, writable: false, configurable: false });"
  " try { Object.defineProperty(window, '__globalRobloxAndroidBridge__', {"
  "   value: bridge, enumerable: true, writable: false, configurable: false });"
  " } catch (_) {} })();";

static GtkApplication *app;
static GtkWindow *window;
static WebKitWebView *view;
static WebKitNetworkSession *session;
static int listener = -1;
static int client = -1;
static guint client_read_source;
static guint client_write_source;
static GByteArray *rx;
static GQueue *tx;
static size_t tx_offset;
static gboolean domain_title;
static gboolean login_page;
static gboolean back_disabled;
static gboolean shutting_down;
static char *last_cookie;

static gboolean flush_tx(gint fd, GIOCondition cond, gpointer data);
static gboolean on_client(gint fd, GIOCondition cond, gpointer data);
static void apply(uint8_t op, const char *payload, size_t size);
static void create_surface(void);
static void destroy_surface(void);

static void drop_client(const char *why) {
  if (client < 0) return;
  fprintf(stderr, "client dropped: %s\n", why);
  if (client_read_source) {
    g_source_remove(client_read_source);
    client_read_source = 0;
  }
  if (client_write_source) {
    g_source_remove(client_write_source);
    client_write_source = 0;
  }
  close(client);
  client = -1;
  g_byte_array_set_size(rx, 0);
  while (!g_queue_is_empty(tx)) g_bytes_unref(g_queue_pop_head(tx));
  tx_offset = 0;
  g_free(last_cookie);
  last_cookie = NULL;
  destroy_surface();
}

static void quit_app(void) {
  if (shutting_down) return;
  shutting_down = TRUE;
  drop_client("shutting down");
  if (app) g_application_quit(G_APPLICATION(app));
}

static void send_event(uint8_t type, const char *payload, size_t size) {
  if (client < 0) return;
  size_t body = 12 + size;
  uint8_t *buffer = g_malloc0(4 + body);
  buffer[0] = body >> 24; buffer[1] = body >> 16; buffer[2] = body >> 8; buffer[3] = body;
  memcpy(buffer + 4, "MWVE", 4);
  buffer[8] = 1;
  buffer[9] = type;
  buffer[12] = size >> 24; buffer[13] = size >> 16; buffer[14] = size >> 8; buffer[15] = size;
  if (size) memcpy(buffer + 16, payload, size);
  g_queue_push_tail(tx, g_bytes_new_take(buffer, 4 + body));
  if (!client_write_source)
    client_write_source = g_unix_fd_add(client, G_IO_OUT | G_IO_HUP | G_IO_ERR, flush_tx, NULL);
}

static gboolean flush_tx(gint fd, GIOCondition cond, gpointer data) {
  (void)data;
  if (cond & (G_IO_HUP | G_IO_ERR)) {
    client_write_source = 0;
    drop_client("write side closed");
    return G_SOURCE_REMOVE;
  }
  while (!g_queue_is_empty(tx)) {
    GBytes *head = g_queue_peek_head(tx);
    gsize length;
    const uint8_t *bytes = g_bytes_get_data(head, &length);
    ssize_t count = send(fd, bytes + tx_offset, length - tx_offset, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (count < 0 && errno == EINTR) continue;
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return G_SOURCE_CONTINUE;
    if (count <= 0) {
      client_write_source = 0;
      drop_client("write failed");
      return G_SOURCE_REMOVE;
    }
    tx_offset += (size_t)count;
    if (tx_offset == length) {
      tx_offset = 0;
      g_bytes_unref(g_queue_pop_head(tx));
    }
  }
  client_write_source = 0;
  return G_SOURCE_REMOVE;
}

static char *user_agent(void) {
  const char *v = getenv("MOCKTAIL_ROBLOX_VERSION");
  if (!v || !*v) v = VERSION;
  return g_strdup_printf(
    "Mozilla/5.0 AppleWebKit/605.1.15 (KHTML, like Gecko)  ROBLOX Android App "
    "%s Tablet Hybrid()  GooglePlayStore RobloxApp/%s (GlobalDist; GooglePlayStore)", v, v);
}

static void on_cookies(GObject *source, GAsyncResult *result, gpointer data) {
  (void)data;
  GError *error = NULL;
  GList *cookies = webkit_cookie_manager_get_cookies_finish(WEBKIT_COOKIE_MANAGER(source), result, &error);
  for (GList *i = cookies; i; i = i->next) {
    SoupCookie *c = i->data;
    const char *name = soup_cookie_get_name(c);
    const char *value = soup_cookie_get_value(c);
    if (name && value && strcmp(name, ".ROBLOSECURITY") == 0 && *value &&
        (!last_cookie || strcmp(last_cookie, value) != 0)) {
      g_free(last_cookie);
      last_cookie = g_strdup(value);
      send_event(EV_COOKIE, value, strlen(value));
      break;
    }
  }
  g_list_free_full(cookies, (GDestroyNotify)soup_cookie_free);
  g_clear_error(&error);
}

static void request_cookie(void) {
  if (!login_page || !session || client < 0) return;
  webkit_cookie_manager_get_cookies(webkit_network_session_get_cookie_manager(session),
                                    "https://www.roblox.com/", NULL, on_cookies, NULL);
}

static void on_cookie_changed(WebKitCookieManager *manager, gpointer data) {
  (void)manager; (void)data;
  request_cookie();
}

static void configure_agent(const char *url) {
  WebKitSettings *settings = webkit_web_view_get_settings(view);
  GUri *uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
  const char *path = uri ? g_uri_get_path(uri) : NULL;
  login_page = path && (strcmp(path, "/login") == 0 || strcmp(path, "/login/") == 0);
  if (uri) g_uri_unref(uri);
  if (login_page) {
    webkit_settings_set_user_agent(settings, NULL);
    request_cookie();
  } else {
    char *agent = user_agent();
    webkit_settings_set_user_agent(settings, agent);
    g_free(agent);
  }
}

static void update_title(void) {
  if (!domain_title || !window || !view) return;
  const char *uri = webkit_web_view_get_uri(view);
  if (!uri) return;
  GUri *parsed = g_uri_parse(uri, G_URI_FLAGS_NONE, NULL);
  if (!parsed) return;
  const char *host = g_uri_get_host(parsed);
  if (host) gtk_window_set_title(window, host);
  g_uri_unref(parsed);
}

static void on_load_changed(WebKitWebView *v, WebKitLoadEvent event, gpointer data) {
  (void)v; (void)data;
  if (event == WEBKIT_LOAD_COMMITTED || event == WEBKIT_LOAD_FINISHED) {
    update_title();
    request_cookie();
  }
}

static GtkWidget *on_create(WebKitWebView *v, WebKitNavigationAction *action, gpointer data) {
  (void)v; (void)data;
  WebKitURIRequest *request = webkit_navigation_action_get_request(action);
  const char *uri = request ? webkit_uri_request_get_uri(request) : NULL;
  if (uri && view) webkit_web_view_load_uri(view, uri);
  return NULL;
}

static char *json_string_after(const char *json, const char *key) {
  char *needle = g_strdup_printf("\"%s\"", key);
  const char *at = strstr(json, needle);
  g_free(needle);
  if (!at) return NULL;
  at = strchr(at + strlen(key) + 2, ':');
  if (!at) return NULL;
  at++;
  while (*at == ' ') at++;
  if (*at != '"') return NULL;
  at++;
  GString *out = g_string_new(NULL);
  while (*at && *at != '"') {
    if (*at == '\\' && at[1]) at++;
    g_string_append_c(out, *at);
    at++;
  }
  return g_string_free(out, FALSE);
}

static void on_callback_done(GObject *source, GAsyncResult *result, gpointer data) {
  gboolean close_after = GPOINTER_TO_INT(data);
  GError *error = NULL;
  JSCValue *value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(source), result, &error);
  gboolean confirmed = !error && value && jsc_value_is_boolean(value) && jsc_value_to_boolean(value);
  if (value) g_object_unref(value);
  g_clear_error(&error);
  if (confirmed && close_after) destroy_surface();
}

static void acknowledge_page_command(const char *json) {
  if (!view || !json) return;
  char *feature = json_string_after(json, "feature");
  char *callback = json_string_after(json, "callbackID");
  if (feature && callback && *callback) {
    gboolean success = strcmp(feature, "CaptchaSuccess") == 0;
    if (success || strcmp(feature, "CaptchaShown") == 0) {
      GString *quoted = g_string_new("\"");
      for (const char *p = callback; *p; p++) {
        if (*p == '"' || *p == '\\') g_string_append_c(quoted, '\\');
        g_string_append_c(quoted, *p);
      }
      g_string_append_c(quoted, '"');
      char *script = g_strdup_printf(
        "(() => { try { if (window.Roblox && window.Roblox.Hybrid && "
        "window.Roblox.Hybrid.Bridge && typeof window.Roblox.Hybrid.Bridge.nativeCallback === 'function') { "
        "window.Roblox.Hybrid.Bridge.nativeCallback(%s, true, {}); return true; } } catch (_) {} return false; })();",
        quoted->str);
      webkit_web_view_evaluate_javascript(view, script, -1, NULL, NULL, NULL,
                                          on_callback_done, GINT_TO_POINTER(success));
      g_free(script);
      g_string_free(quoted, TRUE);
    }
  }
  g_free(feature);
  g_free(callback);
}

static void forward(uint8_t type, JSCValue *value) {
  char *text = NULL;
  if (jsc_value_is_string(value)) text = jsc_value_to_string(value);
  else if (jsc_value_is_object(value)) text = jsc_value_to_json(value, 0);
  if (!text) return;
  size_t size = strlen(text);
  if (size && size <= 64u * 1024u) {
    send_event(type, text, size);
    acknowledge_page_command(text);
  }
  g_free(text);
}

static void on_execute_roblox(WebKitUserContentManager *m, JSCValue *value, gpointer data) {
  (void)m; (void)data;
  forward(EV_EXECUTE_ROBLOX, value);
}

static void on_hybrid(WebKitUserContentManager *m, JSCValue *value, gpointer data) {
  (void)m; (void)data;
  if (!jsc_value_is_object(value)) return;
  JSCValue *command = jsc_value_object_get_property(value, "command");
  if (command) {
    forward(EV_ROBLOX_WK_HYBRID, command);
    g_object_unref(command);
  }
}

static void on_view_close(WebKitWebView *v, gpointer data) {
  (void)v; (void)data;
  destroy_surface();
}

static gboolean on_close_request(GtkWindow *w, gpointer data) {
  (void)w; (void)data;
  window = NULL;
  view = NULL;
  return FALSE;
}

static void on_reload(GtkButton *button, gpointer data) {
  (void)button; (void)data;
  if (view) webkit_web_view_reload_bypass_cache(view);
}

static void on_eval(GObject *source, GAsyncResult *result, gpointer data) {
  (void)data;
  GError *error = NULL;
  JSCValue *value = webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(source), result, &error);
  if (value) g_object_unref(value);
  g_clear_error(&error);
}

static void on_cookie_done(GObject *source, GAsyncResult *result, gpointer data) {
  (void)data;
  GError *error = NULL;
  webkit_cookie_manager_add_cookie_finish(WEBKIT_COOKIE_MANAGER(source), result, &error);
  g_clear_error(&error);
}

static void on_cookie_deleted(GObject *source, GAsyncResult *result, gpointer data) {
  (void)data;
  GError *error = NULL;
  webkit_cookie_manager_delete_cookie_finish(WEBKIT_COOKIE_MANAGER(source), result, &error);
  g_clear_error(&error);
}

static void ensure_session(void) {
  if (session) return;
  gchar *data_dir = g_build_filename(g_get_user_data_dir(), "mocktail", "webview", NULL);
  gchar *cache_dir = g_build_filename(g_get_user_cache_dir(), "mocktail", "webview", NULL);
  g_mkdir_with_parents(data_dir, 0700);
  g_mkdir_with_parents(cache_dir, 0700);
  gchar *cookie_db = g_build_filename(data_dir, "cookies.sqlite", NULL);
  session = webkit_network_session_new(data_dir, cache_dir);
  g_free(data_dir);
  g_free(cache_dir);
  WebKitCookieManager *cookies = webkit_network_session_get_cookie_manager(session);
  webkit_cookie_manager_set_persistent_storage(cookies, cookie_db, WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
  webkit_cookie_manager_set_accept_policy(cookies, WEBKIT_COOKIE_POLICY_ACCEPT_ALWAYS);
  g_signal_connect(cookies, "changed", G_CALLBACK(on_cookie_changed), NULL);
  g_free(cookie_db);
  webkit_network_session_set_itp_enabled(session, FALSE);
  webkit_network_session_set_persistent_credential_storage_enabled(session, TRUE);
}

static void create_surface(void) {
  if (window || view) return;
  ensure_session();

  WebKitUserContentManager *manager = webkit_user_content_manager_new();
  WebKitWebView *created = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "network-session", session,
                                                        "user-content-manager", manager, NULL));
  g_object_unref(manager);
  manager = webkit_web_view_get_user_content_manager(created);
  g_signal_connect(manager, "script-message-received::executeRoblox", G_CALLBACK(on_execute_roblox), NULL);
  g_signal_connect(manager, "script-message-received::RobloxWKHybrid", G_CALLBACK(on_hybrid), NULL);
  g_signal_connect(manager, "script-message-received::mocktailRobloxBridge", G_CALLBACK(on_execute_roblox), NULL);
  webkit_user_content_manager_register_script_message_handler(manager, "executeRoblox", NULL);
  webkit_user_content_manager_register_script_message_handler(manager, "RobloxWKHybrid", NULL);
  webkit_user_content_manager_register_script_message_handler(manager, "mocktailRobloxBridge", NULL);
  WebKitUserScript *script = webkit_user_script_new(kBridge, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                                    WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
  webkit_user_content_manager_add_script(manager, script);
  webkit_user_script_unref(script);

  WebKitSettings *settings = webkit_web_view_get_settings(created);
  webkit_settings_set_enable_javascript(settings, TRUE);
  webkit_settings_set_javascript_can_open_windows_automatically(settings, TRUE);
  webkit_settings_set_enable_back_forward_navigation_gestures(settings, !back_disabled);
  char *agent = user_agent();
  webkit_settings_set_user_agent(settings, agent);
  g_free(agent);

  g_signal_connect(created, "load-changed", G_CALLBACK(on_load_changed), NULL);
  g_signal_connect(created, "create", G_CALLBACK(on_create), NULL);
  g_signal_connect(created, "close", G_CALLBACK(on_view_close), NULL);

  GtkWindow *created_window = GTK_WINDOW(gtk_application_window_new(app));
  gtk_window_set_title(created_window, "Roblox");
  gtk_window_set_default_size(created_window, 1100, 760);
  GtkWidget *header = gtk_header_bar_new();
  GtkWidget *reload = gtk_button_new_from_icon_name("view-refresh-symbolic");
  g_signal_connect(reload, "clicked", G_CALLBACK(on_reload), NULL);
  gtk_header_bar_pack_start(GTK_HEADER_BAR(header), reload);
  gtk_window_set_titlebar(created_window, header);
  gtk_widget_set_vexpand(GTK_WIDGET(created), TRUE);
  gtk_window_set_child(created_window, GTK_WIDGET(created));
  g_signal_connect(created_window, "close-request", G_CALLBACK(on_close_request), NULL);

  view = created;
  window = created_window;
}

static void destroy_surface(void) {
  GtkWindow *w = window;
  window = NULL;
  view = NULL;
  login_page = FALSE;
  domain_title = FALSE;
  if (w) gtk_window_destroy(w);
}

static void apply(uint8_t op, const char *payload, size_t size) {
  if (op == OP_CLOSE) {
    destroy_surface();
    return;
  }
  if (op == OP_SET_BACK_DISABLED) {
    back_disabled = size == 1 && payload[0];
    if (view) webkit_settings_set_enable_back_forward_navigation_gestures(
        webkit_web_view_get_settings(view), !back_disabled);
    return;
  }
  if (op == OP_SHOW_DOMAIN_TITLE) {
    domain_title = size == 1 && payload[0];
    update_title();
    return;
  }
  if (op == OP_SET_COOKIE || op == OP_CLEAR_COOKIE) {
    ensure_session();
    WebKitCookieManager *cookies = webkit_network_session_get_cookie_manager(session);
    if (op == OP_SET_COOKIE) {
      char *value = g_strndup(payload, size);
      SoupCookie *cookie = soup_cookie_new(".ROBLOSECURITY", value, ".roblox.com", "/", -1);
      g_free(value);
      if (cookie) {
        soup_cookie_set_secure(cookie, TRUE);
        soup_cookie_set_http_only(cookie, TRUE);
        soup_cookie_set_same_site_policy(cookie, SOUP_SAME_SITE_POLICY_LAX);
        webkit_cookie_manager_add_cookie(cookies, cookie, NULL, on_cookie_done, NULL);
        soup_cookie_free(cookie);
      }
    } else {
      SoupCookie *cookie = soup_cookie_new(".ROBLOSECURITY", "", ".roblox.com", "/", -1);
      if (cookie) {
        webkit_cookie_manager_delete_cookie(cookies, cookie, NULL, on_cookie_deleted, NULL);
        soup_cookie_free(cookie);
      }
    }
    return;
  }
  if (op == OP_RETAIN_COOKIE) return;
  if (op == OP_LOAD_URL) create_surface();
  if (!window || !view) return;
  switch (op) {
    case OP_LOAD_URL: {
      char *url = g_strndup(payload, size);
      configure_agent(url);
      webkit_web_view_load_uri(view, url);
      gtk_window_present(window);
      g_free(url);
      break;
    }
    case OP_SET_TITLE: {
      char *title = g_strndup(payload, size);
      gtk_window_set_title(window, title);
      g_free(title);
      break;
    }
    case OP_SET_VISIBLE:
      if (size == 1 && payload[0]) gtk_window_present(window);
      else gtk_widget_set_visible(GTK_WIDGET(window), FALSE);
      break;
    case OP_EVAL_JS:
      webkit_web_view_evaluate_javascript(view, payload, (gssize)size, NULL, NULL, NULL, on_eval, NULL);
      break;
    default:
      break;
  }
}

static gboolean process_frames(void) {
  while (rx->len >= 4) {
    uint32_t length = ((uint32_t)rx->data[0] << 24) | ((uint32_t)rx->data[1] << 16) |
                      ((uint32_t)rx->data[2] << 8) | rx->data[3];
    if (length > MAX_PACKET) return FALSE;
    if (rx->len < 4u + length) break;
    uint8_t *copy = g_memdup2(rx->data + 4, length);
    g_byte_array_remove_range(rx, 0, 4 + length);
    if (length >= 12 && memcmp(copy, "MWVC", 4) == 0) {
      size_t size = ((size_t)copy[8] << 24) | ((size_t)copy[9] << 16) |
                    ((size_t)copy[10] << 8) | copy[11];
      if (size == length - 12) apply(copy[5], (const char *)copy + 12, size);
    }
    g_free(copy);
    if (client < 0) return TRUE;
  }
  return TRUE;
}

static gboolean on_client(gint fd, GIOCondition cond, gpointer data) {
  (void)data;
  uint8_t buffer[8192];
  while (TRUE) {
    ssize_t count = read(fd, buffer, sizeof buffer);
    if (count < 0 && errno == EINTR) continue;
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
    if (count <= 0) {
      client_read_source = 0;
      drop_client("client closed");
      return G_SOURCE_REMOVE;
    }
    g_byte_array_append(rx, buffer, (guint)count);
    if (!process_frames()) {
      client_read_source = 0;
      drop_client("bad frame");
      return G_SOURCE_REMOVE;
    }
    if (client < 0) return G_SOURCE_REMOVE;
  }
  if (cond & (G_IO_HUP | G_IO_ERR)) {
    client_read_source = 0;
    drop_client("client hung up");
    return G_SOURCE_REMOVE;
  }
  return G_SOURCE_CONTINUE;
}

static gboolean on_listener(gint fd, GIOCondition cond, gpointer data) {
  (void)cond; (void)data;
  int accepted = accept(fd, NULL, NULL);
  if (accepted < 0) return G_SOURCE_CONTINUE;
  if (client >= 0) drop_client("replaced by a new connection");
  fcntl(accepted, F_SETFD, FD_CLOEXEC);
  fcntl(accepted, F_SETFL, fcntl(accepted, F_GETFL) | O_NONBLOCK);
  int one = 1;
  setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
  client = accepted;
  fprintf(stderr, "client connected\n");
  client_read_source = g_unix_fd_add(client, G_IO_IN | G_IO_HUP | G_IO_ERR, on_client, NULL);
  send_event(EV_READY, "", 0);
  return G_SOURCE_CONTINUE;
}

static int listen_socket(void) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  struct sockaddr_in address;
  memset(&address, 0, sizeof address);
  address.sin_family = AF_INET;
  address.sin_port = htons(PORT);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(fd, (struct sockaddr *)&address, sizeof address) != 0 || listen(fd, 4) != 0) {
    close(fd);
    return -1;
  }
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  return fd;
}

static void activate(GtkApplication *application, gpointer data) {
  (void)application; (void)data;
}

static gboolean on_sigterm(gpointer data) {
  (void)data;
  quit_app();
  return G_SOURCE_REMOVE;
}

int main(int argc, char **argv) {
  (void)argc;
  setenv("WEBKIT_DISABLE_DMABUF_RENDERER", "1", 0);
  setenv("WEBKIT_DISABLE_COMPOSITING_MODE", "1", 0);
  rx = g_byte_array_new();
  tx = g_queue_new();
  listener = listen_socket();
  if (listener < 0) {
    fprintf(stderr, "cannot listen on 127.0.0.1:%d: %s\n", PORT, strerror(errno));
    return 1;
  }
  g_unix_fd_add(listener, G_IO_IN, on_listener, NULL);
  g_unix_signal_add(SIGTERM, on_sigterm, NULL);
  g_unix_signal_add(SIGINT, on_sigterm, NULL);
  app = gtk_application_new("org.mocktail.WebViewHost", G_APPLICATION_NON_UNIQUE);
  g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
  g_application_hold(G_APPLICATION(app));
  int status = g_application_run(G_APPLICATION(app), 1, argv);
  g_object_unref(app);
  return status;
}
