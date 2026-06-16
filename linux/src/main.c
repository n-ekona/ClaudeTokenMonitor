// Claude Code トークン監視 — ネイティブ Linux アプリ (GTK3 + WebKitGTK)。
// ローカルサーバーもポートも使わず、WebKitWebView 内にダッシュボードを表示し、
// stats が集計した値を evaluate_javascript で直接流し込む。

#define _GNU_SOURCE   // readlink の宣言を有効化

#include <gtk/gtk.h>
#include <webkit2/webkit2.h>
#include <json-glib/json-glib.h>
#include <limits.h>
#include <unistd.h>
#include <string.h>

#include "stats.h"

typedef struct {
    Stats *stats;
    WebKitWebView *web;
    guint timeout_id;
    gboolean initialized;
} App;

// 集計スナップショットをページへ配信。
// JSON はそのまま JS の式（オブジェクトリテラル）として評価できる。
static void push_snapshot(App *app) {
    if (!app->web) return;
    char *json = stats_snapshot_json(app->stats);
    char *script = g_strdup_printf("window.__recv(%s)", json);
    // run_javascript は古い WebKitGTK でも利用可能（evaluate_javascript は 2.40+ 限定）
    webkit_web_view_run_javascript(app->web, script, NULL, NULL, NULL);
    g_free(script);
    g_free(json);
}

static gboolean tick_cb(gpointer data) {
    App *app = data;
    stats_tick(app->stats);
    push_snapshot(app);
    return G_SOURCE_CONTINUE;
}

static void schedule_timer(App *app) {
    if (app->timeout_id) g_source_remove(app->timeout_id);
    app->timeout_id = g_timeout_add(stats_interval_ms(app->stats), tick_cb, app);
}

// ページ → ネイティブ:
//   "ready"                      → 初回スナップショット送信
//   {"type":"interval","ms":N}   → 更新間隔を変更
static void on_message(WebKitUserContentManager *ucm, WebKitJavascriptResult *res, gpointer data) {
    (void)ucm;
    App *app = data;
    JSCValue *val = webkit_javascript_result_get_js_value(res);
    if (jsc_value_is_string(val)) {
        char *s = jsc_value_to_string(val);
        if (s && g_str_has_prefix(g_strchug(s), "{")) {
            JsonParser *p = json_parser_new();
            if (json_parser_load_from_data(p, s, -1, NULL)) {
                JsonNode *rn = json_parser_get_root(p);
                if (rn && JSON_NODE_HOLDS_OBJECT(rn)) {
                    JsonObject *o = json_node_get_object(rn);
                    if (g_strcmp0(json_object_get_string_member_with_default(o, "type", ""), "interval") == 0 &&
                        json_object_has_member(o, "ms")) {
                        int ms = (int)json_object_get_int_member(o, "ms");
                        stats_set_interval(app->stats, ms);
                        schedule_timer(app); // 新しい間隔で再スケジュール
                    }
                }
            }
            g_object_unref(p);
        }
        g_free(s);
    }
    push_snapshot(app);
}

static void on_load_changed(WebKitWebView *web, WebKitLoadEvent ev, gpointer data) {
    (void)web;
    App *app = data;
    if (ev != WEBKIT_LOAD_FINISHED) return;
    if (!app->initialized) {
        app->initialized = TRUE;
        stats_full_scan(app->stats);
        schedule_timer(app);
    }
    push_snapshot(app);
}

// 実行ファイル隣の web/index.html を探す
static char *locate_index_uri(void) {
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    char *idx = NULL;
    if (n > 0) {
        exe[n] = '\0';
        char *dir = g_path_get_dirname(exe);
        idx = g_build_filename(dir, "web", "index.html", NULL);
        g_free(dir);
    }
    if (!idx || !g_file_test(idx, G_FILE_TEST_EXISTS)) {
        g_free(idx);
        idx = g_build_filename(g_get_current_dir(), "web", "index.html", NULL); // CWD フォールバック
    }
    char *uri = g_filename_to_uri(idx, NULL, NULL);
    g_free(idx);
    return uri;
}

int main(int argc, char **argv) {
    gtk_init(&argc, &argv);

    App *app = g_new0(App, 1);
    app->stats = stats_new();

    WebKitUserContentManager *ucm = webkit_user_content_manager_new();
    webkit_user_content_manager_register_script_message_handler(ucm, "tokenmonitor");
    g_signal_connect(ucm, "script-message-received::tokenmonitor", G_CALLBACK(on_message), app);

    app->web = WEBKIT_WEB_VIEW(webkit_web_view_new_with_user_content_manager(ucm));

    // ローカルファイル配信 & 為替APIへの fetch を許可（ローカル完結の信頼アプリ）
    WebKitSettings *st = webkit_web_view_get_settings(app->web);
    webkit_settings_set_allow_file_access_from_file_urls(st, TRUE);
    webkit_settings_set_allow_universal_access_from_file_urls(st, TRUE);

    g_signal_connect(app->web, "load-changed", G_CALLBACK(on_load_changed), app);

    GtkWidget *win = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(win), "Claude Code トークン監視");
    gtk_window_set_default_size(GTK_WINDOW(win), 1240, 860);
    g_signal_connect(win, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    gtk_container_add(GTK_CONTAINER(win), GTK_WIDGET(app->web));
    gtk_widget_show_all(win);

    char *uri = locate_index_uri();
    if (uri) {
        webkit_web_view_load_uri(app->web, uri);
        g_free(uri);
    }

    gtk_main();

    if (app->timeout_id) g_source_remove(app->timeout_id);
    stats_free(app->stats);
    g_free(app);
    return 0;
}
