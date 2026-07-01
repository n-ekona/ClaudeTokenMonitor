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
    GtkWidget *win;
    guint timeout_id;
    gboolean initialized;

    // PiP（正方形）モード。通常時のサイズ/位置/装飾を退避して復元する。
    gboolean pip_on;
    int prev_w, prev_h, prev_x, prev_y;
    gboolean prev_decorated;
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

// ネイティブ枠の配色をテーマに合わせる（スクロールバーは WebView の color-scheme が追従）
static void apply_theme(gboolean dark) {
    GtkSettings *gs = gtk_settings_get_default();
    if (gs) g_object_set(gs, "gtk-application-prefer-dark-theme", dark, NULL);
}

// PiP ウィンドウを、自分が乗っているモニタの作業領域の右下隅(24px マージン)へ移動。
static void pip_move_to_corner(GtkWindow *win, int size) {
    GdkDisplay *display = gtk_widget_get_display(GTK_WIDGET(win));
    GdkMonitor *mon = NULL;
    GdkWindow *gw = gtk_widget_get_window(GTK_WIDGET(win));
    if (gw) mon = gdk_display_get_monitor_at_window(display, gw);
    if (!mon) mon = gdk_display_get_primary_monitor(display);
    if (!mon) mon = gdk_display_get_monitor(display, 0);
    if (!mon) return;
    GdkRectangle wa;
    gdk_monitor_get_workarea(mon, &wa);
    int x = wa.x + wa.width - size - 24;
    int y = wa.y + wa.height - size - 24;
    gtk_window_move(win, x, y);
}

// 正方形(PiP)モードの切替。on で装飾なし最前面の正方形へ、off で元へ戻す。
static void set_pip(App *app, gboolean on, int size, int opacity) {
    if (size < 240) size = 240;
    if (size > 520) size = 520;
    if (opacity < 30) opacity = 30;
    if (opacity > 100) opacity = 100;
    GtkWindow *win = GTK_WINDOW(app->win);

    if (on && !app->pip_on) {
        // 通常表示の復元用に現在のサイズ/位置/装飾を退避
        gtk_window_get_size(win, &app->prev_w, &app->prev_h);
        gtk_window_get_position(win, &app->prev_x, &app->prev_y);
        app->prev_decorated = gtk_window_get_decorated(win);

        gtk_window_set_decorated(win, FALSE); // タイトルバー/枠を消す
        gtk_window_set_keep_above(win, TRUE); // 常に最前面
        gtk_window_resize(win, size, size);
        pip_move_to_corner(win, size);
        app->pip_on = TRUE;
    } else if (on && app->pip_on) {
        // 既に PiP 表示中にサイズだけ変わった場合は、その場でリサイズして配置し直す
        gtk_window_resize(win, size, size);
        pip_move_to_corner(win, size);
    } else if (!on && app->pip_on) {
        gtk_window_set_keep_above(win, FALSE);
        gtk_window_set_decorated(win, app->prev_decorated);
        gtk_window_resize(win, app->prev_w, app->prev_h);
        gtk_window_move(win, app->prev_x, app->prev_y);
        app->pip_on = FALSE;
    }

    // PiP 中だけ窓の不透明度を反映。通常表示へ戻したら不透明(1.0)へ。
    gtk_widget_set_opacity(GTK_WIDGET(win), app->pip_on ? (opacity / 100.0) : 1.0);
}

// フチなし PiP でもウィンドウを掴んで動かせるよう、OS のウィンドウ移動ループを起動。
static void pip_begin_drag(App *app) {
    GtkWindow *win = GTK_WINDOW(app->win);
    GdkDisplay *display = gtk_widget_get_display(GTK_WIDGET(win));
    GdkSeat *seat = gdk_display_get_default_seat(display);
    GdkDevice *pointer = seat ? gdk_seat_get_pointer(seat) : NULL;
    int x = 0, y = 0;
    if (pointer) gdk_device_get_position(pointer, NULL, &x, &y); // ルート座標
    gtk_window_begin_move_drag(win, 1 /* 左ボタン */, x, y, GDK_CURRENT_TIME);
}

// JSON を window.__recv(...) へ splice する前に、U+2028/U+2029（JSON では有効だが
// ES2019 以前の JS では行終端）を  /  へ逃がす。新規確保で返す。
static char *escape_js_line_seps(const char *s) {
    GString *out = g_string_new(NULL);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (p[0] == 0xE2 && p[1] == 0x80 && (p[2] == 0xA8 || p[2] == 0xA9)) {
            g_string_append(out, p[2] == 0xA8 ? "\\u2028" : "\\u2029");
            p += 2;
        } else {
            g_string_append_c(out, (char)*p);
        }
    }
    return g_string_free(out, FALSE);
}

// カスタムテーマを JSON で保存（既定フォルダは ~/Documents）。
static void export_theme(App *app, const char *json, const char *filename) {
    GtkWidget *d = gtk_file_chooser_dialog_new(
        "保存", GTK_WINDOW(app->win), GTK_FILE_CHOOSER_ACTION_SAVE,
        "キャンセル", GTK_RESPONSE_CANCEL, "保存", GTK_RESPONSE_ACCEPT, NULL);
    GtkFileChooser *ch = GTK_FILE_CHOOSER(d);
    const char *docs = g_get_user_special_dir(G_USER_DIRECTORY_DOCUMENTS);
    if (docs) gtk_file_chooser_set_current_folder(ch, docs);
    gtk_file_chooser_set_current_name(ch, (filename && *filename) ? filename : "theme.json");
    gtk_file_chooser_set_do_overwrite_confirmation(ch, TRUE);
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
        char *fn = gtk_file_chooser_get_filename(ch);
        if (fn) { g_file_set_contents(fn, json ? json : "", -1, NULL); g_free(fn); }
    }
    gtk_widget_destroy(d);
}

// JSON を読み込み、object なら web へ返す（既定フォルダは ~/Documents）。
static void import_theme(App *app) {
    GtkWidget *d = gtk_file_chooser_dialog_new(
        "読み込み", GTK_WINDOW(app->win), GTK_FILE_CHOOSER_ACTION_OPEN,
        "キャンセル", GTK_RESPONSE_CANCEL, "開く", GTK_RESPONSE_ACCEPT, NULL);
    GtkFileChooser *ch = GTK_FILE_CHOOSER(d);
    const char *docs = g_get_user_special_dir(G_USER_DIRECTORY_DOCUMENTS);
    if (docs) gtk_file_chooser_set_current_folder(ch, docs);
    GtkFileFilter *filter = gtk_file_filter_new();
    gtk_file_filter_set_name(filter, "JSON (*.json)");
    gtk_file_filter_add_pattern(filter, "*.json");
    gtk_file_chooser_add_filter(ch, filter);
    if (gtk_dialog_run(GTK_DIALOG(d)) == GTK_RESPONSE_ACCEPT) {
        char *fn = gtk_file_chooser_get_filename(ch);
        char *contents = NULL;
        if (fn && g_file_get_contents(fn, &contents, NULL, NULL)) {
            JsonParser *p = json_parser_new();
            if (json_parser_load_from_data(p, contents, -1, NULL)) {
                JsonNode *rn = json_parser_get_root(p);
                if (rn && JSON_NODE_HOLDS_OBJECT(rn)) {
                    JsonGenerator *gen = json_generator_new();
                    json_generator_set_root(gen, rn);
                    char *reser = json_generator_to_data(gen, NULL);
                    char *safe = escape_js_line_seps(reser); // 旧 JS エンジンでの行終端誤認を防ぐ
                    char *script = g_strdup_printf(
                        "window.__recv({\"type\":\"importedTheme\",\"theme\":%s})", safe);
                    webkit_web_view_run_javascript(app->web, script, NULL, NULL, NULL);
                    g_free(script); g_free(safe); g_free(reser); g_object_unref(gen);
                }
            }
            g_object_unref(p);
        }
        g_free(contents);
        g_free(fn);
    }
    gtk_widget_destroy(d);
}

// ページ → ネイティブ:
//   "ready"                       → 初回スナップショット送信
//   {"type":"interval","ms":N}    → 更新間隔を変更
//   {"type":"projectsDir","path"} → 監視対象パスの変更＋再スキャン
//   {"type":"rescan"}             → 集計を全クリアして再スキャン
//   {"type":"pricing","table":[]} → モデル単価の上書き＋再計算
//   {"type":"theme","mode":"dark"}→ ネイティブ枠の配色
//   {"type":"pip","on":b,"size":N}→ 正方形(PiP)モードの切替
//   {"type":"drag"}               → PiP ウィンドウのドラッグ開始
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
                    const char *type = json_object_get_string_member_with_default(o, "type", "");
                    if (g_strcmp0(type, "interval") == 0 && json_object_has_member(o, "ms")) {
                        int ms = (int)json_object_get_int_member(o, "ms");
                        stats_set_interval(app->stats, ms);
                        schedule_timer(app); // 新しい間隔で再スケジュール
                    } else if (g_strcmp0(type, "projectsDir") == 0) {
                        const char *path = json_object_get_string_member_with_default(o, "path", NULL);
                        stats_set_projects_dir(app->stats, path);
                    } else if (g_strcmp0(type, "rescan") == 0) {
                        stats_rescan(app->stats);
                    } else if (g_strcmp0(type, "pricing") == 0 && json_object_has_member(o, "table")) {
                        JsonNode *tn = json_object_get_member(o, "table");
                        if (JSON_NODE_HOLDS_ARRAY(tn))
                            stats_set_pricing(app->stats, json_node_get_array(tn));
                    } else if (g_strcmp0(type, "ui") == 0 && json_object_has_member(o, "settings")) {
                        JsonNode *sn = json_object_get_member(o, "settings");
                        if (JSON_NODE_HOLDS_OBJECT(sn))
                            stats_set_ui(app->stats, sn);
                    } else if (g_strcmp0(type, "exportTheme") == 0) {
                        const char *json = json_object_get_string_member_with_default(o, "json", NULL);
                        const char *filename = json_object_get_string_member_with_default(o, "filename", "theme.json");
                        if (json) export_theme(app, json, filename);
                    } else if (g_strcmp0(type, "importTheme") == 0) {
                        import_theme(app);
                    } else if (g_strcmp0(type, "theme") == 0) {
                        const char *mode = json_object_get_string_member_with_default(o, "mode", NULL);
                        apply_theme(g_strcmp0(mode, "light") != 0); // 既定はダーク
                    } else if (g_strcmp0(type, "pip") == 0 && json_object_has_member(o, "on")) {
                        gboolean on = json_object_get_boolean_member_with_default(o, "on", FALSE);
                        int size = (int)json_object_get_int_member_with_default(o, "size", 320);
                        int opacity = (int)json_object_get_int_member_with_default(o, "opacity", 100);
                        set_pip(app, on, size, opacity);
                    } else if (g_strcmp0(type, "drag") == 0) {
                        if (app->pip_on) pip_begin_drag(app);
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

// 実行ファイル隣の web/icon.png をウィンドウ/タスクバーのアイコンに設定（無ければ無視）
static void set_window_icon(GtkWindow *win) {
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    char *path = NULL;
    if (n > 0) {
        exe[n] = '\0';
        char *dir = g_path_get_dirname(exe);
        path = g_build_filename(dir, "web", "icon.png", NULL);
        g_free(dir);
    }
    if (!path || !g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_free(path);
        path = g_build_filename(g_get_current_dir(), "web", "icon.png", NULL); // CWD フォールバック
    }
    if (g_file_test(path, G_FILE_TEST_EXISTS))
        gtk_window_set_icon_from_file(win, path, NULL);
    g_free(path);
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
    app->win = win;
    gtk_window_set_title(GTK_WINDOW(win), "Claude Code トークン監視");
    set_window_icon(GTK_WINDOW(win));
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
