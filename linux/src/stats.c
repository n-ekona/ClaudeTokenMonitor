#define _GNU_SOURCE   // timegm / localtime_r の宣言を有効化

#include "stats.h"

#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>

// ---- 料金表 (per MTok: input, output) ------------------------------------
// 既定14件（不変）。実効テーブルは config / UI で上書き可能（s->pricing）。
typedef struct { const char *key; double in_p; double out_p; } Price;
static const Price DEFAULT_PRICING[] = {
    {"claude-fable-5", 10, 50},
    {"claude-mythos-5", 10, 50},
    {"claude-opus-4-8", 5, 25},
    {"claude-opus-4-7", 5, 25},
    {"claude-opus-4-6", 5, 25},
    {"claude-opus-4-5", 5, 25},
    {"claude-opus-4-1", 15, 75},
    {"claude-opus-4-0", 15, 75},
    {"claude-sonnet-4-6", 3, 15},
    {"claude-sonnet-4-5", 3, 15},
    {"claude-sonnet-4-0", 3, 15},
    {"claude-haiku-4-5", 1, 5},
    {"claude-3-5-haiku", 0.8, 4},
    {"claude-3-haiku", 0.25, 1.25},
};

// 実効テーブルの1要素（key は所有・g_free 対象）
typedef struct { char *key; double in_p; double out_p; } PriceEntry;

#define CACHE_WRITE_5M_MULT 1.25
#define CACHE_WRITE_1H_MULT 2.0
#define CACHE_READ_MULT 0.1

typedef struct {
    gint64 input, output, cw5, cw1, cread, messages;
    double cost;
} Bucket;

struct _Stats {
    char *projects_dir;
    char *default_projects_dir;  // 既定の監視ルート(空指定で戻す先)
    char *home_dir;          // displayPath の ~ 短縮基準(projects_dir 上書きと無関係に実 home)
    char *config_path;       // config.json の絶対パス
    int interval_ms;
    GArray *pricing;         // PriceEntry の動的配列（実効の料金表）

    Bucket total;
    GHashTable *by_model;    // char* -> Bucket*
    GHashTable *by_project;  // char* -> Bucket*
    GHashTable *by_day;      // "yyyy-MM-dd" -> Bucket*
    // フォルダ(プロジェクトキー) -> 真のルート cwd / それが encode(cwd)==folder か。移動統合用。
    GHashTable *by_project_canonical;        // char* folder -> char* cwd
    GHashTable *by_project_canonical_match;  // char* folder -> gboolean(GINT_TO_POINTER)
    GHashTable *seen_ids;    // set of char*
    GHashTable *offsets;     // char* path -> gint64*
    char *last_activity;
    char *started_at;
};

static gint64 bucket_tokens(const Bucket *b) {
    return b->input + b->output + b->cw5 + b->cw1 + b->cread;
}

static void price_for(Stats *s, const char *model, double *in_p, double *out_p) {
    for (guint i = 0; i < s->pricing->len; i++) {
        const PriceEntry *e = &g_array_index(s->pricing, PriceEntry, i);
        if (g_strcmp0(model, e->key) == 0 || g_str_has_prefix(model, e->key)) {
            *in_p = e->in_p;
            *out_p = e->out_p;
            return;
        }
    }
    *in_p = 5;  // フォールバック
    *out_p = 25;
}

// GArray<PriceEntry> の各要素 key を解放（g_array_set_clear_func 用）
static void price_entry_clear(gpointer p) {
    PriceEntry *e = p;
    g_free(e->key);
    e->key = NULL;
}

// 実効テーブルを既定14件で初期化する。
static void pricing_set_defaults(Stats *s) {
    g_array_set_size(s->pricing, 0); // clear_func が既存 key を解放
    for (gsize i = 0; i < G_N_ELEMENTS(DEFAULT_PRICING); i++) {
        PriceEntry e = { g_strdup(DEFAULT_PRICING[i].key), DEFAULT_PRICING[i].in_p, DEFAULT_PRICING[i].out_p };
        g_array_append_val(s->pricing, e);
    }
}

// JSON オブジェクトから数値メンバを double で取得（int でも double でも可）
static double get_double(JsonObject *obj, const char *name) {
    if (!json_object_has_member(obj, name)) return 0;
    JsonNode *n = json_object_get_member(obj, name);
    if (!JSON_NODE_HOLDS_VALUE(n)) return 0;
    GType t = json_node_get_value_type(n);
    if (t == G_TYPE_INT64) return (double)json_node_get_int(n);
    if (t == G_TYPE_DOUBLE) return json_node_get_double(n);
    return 0;
}

// JsonArray から実効テーブルを上書き構築。1件以上で TRUE。
// 空/NULL/全無効なら s->pricing は変更せず FALSE を返す。
static gboolean pricing_set_from_array(Stats *s, JsonArray *arr) {
    if (!arr) return FALSE;
    guint n = json_array_get_length(arr);
    GArray *tmp = g_array_new(FALSE, FALSE, sizeof(PriceEntry));
    for (guint i = 0; i < n; i++) {
        JsonNode *node = json_array_get_element(arr, i);
        if (!node || !JSON_NODE_HOLDS_OBJECT(node)) continue;
        JsonObject *o = json_node_get_object(node);
        const char *key = json_object_get_string_member_with_default(o, "key", NULL);
        if (!key || !*key) continue;
        PriceEntry e = { g_strdup(key), get_double(o, "in"), get_double(o, "out") };
        g_array_append_val(tmp, e);
    }
    if (tmp->len == 0) { g_array_free(tmp, TRUE); return FALSE; }

    // 既存を解放してから所有権ごと差し替え
    g_array_set_size(s->pricing, 0); // clear_func が既存 key を解放
    for (guint i = 0; i < tmp->len; i++) {
        PriceEntry *e = &g_array_index(tmp, PriceEntry, i);
        g_array_append_val(s->pricing, *e); // key の所有権を移譲
    }
    // tmp には clear_func 未設定。TRUE で配列バッファのみ解放（key は s->pricing へ移譲済み）。
    g_array_free(tmp, TRUE);
    return TRUE;
}

static char *iso_now(void) {
    GDateTime *dt = g_date_time_new_now_utc();
    char *frac = g_date_time_format(dt, "%Y-%m-%dT%H:%M:%S");
    char *out = g_strdup_printf("%s.%03dZ", frac, g_date_time_get_microsecond(dt) / 1000);
    g_free(frac);
    g_date_time_unref(dt);
    return out;
}

// ---- 実効テーブルを JsonBuilder へ [{key,in,out}] で書き出す ----------------
static void add_pricing_array(JsonBuilder *b, Stats *s) {
    json_builder_begin_array(b);
    for (guint i = 0; i < s->pricing->len; i++) {
        const PriceEntry *e = &g_array_index(s->pricing, PriceEntry, i);
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "key"); json_builder_add_string_value(b, e->key);
        json_builder_set_member_name(b, "in");  json_builder_add_double_value(b, e->in_p);
        json_builder_set_member_name(b, "out"); json_builder_add_double_value(b, e->out_p);
        json_builder_end_object(b);
    }
    json_builder_end_array(b);
}

// ---- 設定ファイル(config.json)の読み書き --------------------------------
// 起動時に呼ぶ。存在すれば projectsDir / intervalMs / pricing を上書きする。
static void load_config(Stats *s) {
    if (!g_file_test(s->config_path, G_FILE_TEST_EXISTS)) return;
    JsonParser *p = json_parser_new();
    if (json_parser_load_from_file(p, s->config_path, NULL)) {
        JsonNode *rn = json_parser_get_root(p);
        if (rn && JSON_NODE_HOLDS_OBJECT(rn)) {
            JsonObject *o = json_node_get_object(rn);
            const char *pd = json_object_get_string_member_with_default(o, "projectsDir", NULL);
            if (pd && *pd) {
                g_free(s->projects_dir);
                s->projects_dir = g_strdup(pd);
            }
            if (json_object_has_member(o, "intervalMs")) {
                JsonNode *iv = json_object_get_member(o, "intervalMs");
                if (JSON_NODE_HOLDS_VALUE(iv))
                    s->interval_ms = CLAMP((int)json_node_get_int(iv), 500, 600000);
            }
            if (json_object_has_member(o, "pricing")) {
                JsonNode *pr = json_object_get_member(o, "pricing");
                if (JSON_NODE_HOLDS_ARRAY(pr))
                    pricing_set_from_array(s, json_node_get_array(pr)); // 空なら既定のまま
            }
        }
    }
    g_object_unref(p);
}

// 変更操作のたびに保存。失敗は致命的ではないので無視する。
static void save_config(Stats *s) {
    char *dir = g_path_get_dirname(s->config_path);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);

    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "projectsDir"); json_builder_add_string_value(b, s->projects_dir);
    json_builder_set_member_name(b, "intervalMs");  json_builder_add_int_value(b, s->interval_ms);
    json_builder_set_member_name(b, "pricing");     add_pricing_array(b, s);
    json_builder_end_object(b);

    JsonGenerator *gen = json_generator_new();
    JsonNode *root = json_builder_get_root(b);
    json_generator_set_root(gen, root);
    json_generator_to_file(gen, s->config_path, NULL);
    json_node_free(root);
    g_object_unref(gen);
    g_object_unref(b);
}

// 集計状態を全消去（パス変更・再スキャン・単価変更で使う）。
static void clear_state(Stats *s) {
    memset(&s->total, 0, sizeof(Bucket));
    g_hash_table_remove_all(s->by_model);
    g_hash_table_remove_all(s->by_project);
    g_hash_table_remove_all(s->by_day);
    g_hash_table_remove_all(s->by_project_canonical);
    g_hash_table_remove_all(s->by_project_canonical_match);
    g_hash_table_remove_all(s->seen_ids);
    g_hash_table_remove_all(s->offsets);
    g_free(s->last_activity);
    s->last_activity = NULL;
}

Stats *stats_new(void) {
    Stats *s = g_new0(Stats, 1);

    s->home_dir = g_strdup(g_get_home_dir());
    const char *dir = g_getenv("CLAUDE_PROJECTS_DIR");
    if (dir && *dir) {
        s->default_projects_dir = g_strdup(dir);
    } else {
        s->default_projects_dir = g_build_filename(s->home_dir, ".claude", "projects", NULL);
    }
    s->projects_dir = g_strdup(s->default_projects_dir);

    const char *ms = g_getenv("REFRESH_INTERVAL_MS");
    int v = ms ? atoi(ms) : 0;
    s->interval_ms = (ms && v > 0) ? CLAMP(v, 500, 600000) : 3000;

    // config.json は ${XDG_CONFIG_HOME:-~/.config}/ClaudeTokenMonitor/ に置く
    s->config_path = g_build_filename(g_get_user_config_dir(), "ClaudeTokenMonitor", "config.json", NULL);

    // 実効の料金表（既定14件で初期化。config / UI で上書き可能）
    s->pricing = g_array_new(FALSE, FALSE, sizeof(PriceEntry));
    g_array_set_clear_func(s->pricing, price_entry_clear);
    pricing_set_defaults(s);

    s->by_model = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    s->by_project = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    s->by_day = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    s->by_project_canonical = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    s->by_project_canonical_match = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    s->seen_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    s->offsets = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    s->started_at = iso_now();

    load_config(s); // 設定ファイルがあれば projectsDir / intervalMs / pricing を上書き
    return s;
}

void stats_free(Stats *s) {
    if (!s) return;
    g_hash_table_destroy(s->by_model);
    g_hash_table_destroy(s->by_project);
    g_hash_table_destroy(s->by_day);
    g_hash_table_destroy(s->by_project_canonical);
    g_hash_table_destroy(s->by_project_canonical_match);
    g_hash_table_destroy(s->seen_ids);
    g_hash_table_destroy(s->offsets);
    g_array_free(s->pricing, TRUE); // clear_func が各 key を解放
    g_free(s->projects_dir);
    g_free(s->default_projects_dir);
    g_free(s->home_dir);
    g_free(s->config_path);
    g_free(s->last_activity);
    g_free(s->started_at);
    g_free(s);
}

const char *stats_projects_dir(Stats *s) { return s->projects_dir; }
int stats_interval_ms(Stats *s) { return s->interval_ms; }
void stats_set_interval(Stats *s, int ms) {
    s->interval_ms = CLAMP(ms, 500, 600000);
    save_config(s);
}

// 監視対象パスを変更し、全クリアして再スキャン＋config 保存。空/NULL は既定へ。
void stats_set_projects_dir(Stats *s, const char *path) {
    char *trimmed = path ? g_strdup(path) : NULL;
    if (trimmed) g_strstrip(trimmed);
    g_free(s->projects_dir);
    if (!trimmed || !*trimmed)
        s->projects_dir = g_strdup(s->default_projects_dir);
    else
        s->projects_dir = g_strdup(trimmed);
    g_free(trimmed);

    clear_state(s);
    stats_full_scan(s);
    save_config(s);
}

// 集計を全クリアして再スキャン（リセット）。
void stats_rescan(Stats *s) {
    clear_state(s);
    stats_full_scan(s);
}

// モデル単価を上書きし、コスト再計算のため全クリアして再スキャン＋config 保存。
void stats_set_pricing(Stats *s, JsonArray *table) {
    if (!pricing_set_from_array(s, table))
        pricing_set_defaults(s); // 空/NULL/全無効なら既定14件へ
    clear_state(s);
    stats_full_scan(s);
    save_config(s);
}

static Bucket *get_bucket(GHashTable *map, const char *key) {
    Bucket *b = g_hash_table_lookup(map, key);
    if (!b) {
        b = g_new0(Bucket, 1);
        g_hash_table_insert(map, g_strdup(key), b);
    }
    return b;
}

static void apply(Bucket *b, gint64 input, gint64 output, gint64 w5, gint64 w1, gint64 cread, double cost) {
    b->input += input;
    b->output += output;
    b->cw5 += w5;
    b->cw1 += w1;
    b->cread += cread;
    b->cost += cost;
    b->messages += 1;
}

// JSON オブジェクトから整数メンバを安全に取得
static gint64 get_int(JsonObject *obj, const char *name) {
    if (!json_object_has_member(obj, name)) return 0;
    JsonNode *n = json_object_get_member(obj, name);
    if (JSON_NODE_HOLDS_VALUE(n)) return json_node_get_int(n);
    return 0;
}

// timestamp(ISO8601, UTC) → ローカル日付 "yyyy-MM-dd"。失敗時 NULL。
static char *local_day_key(const char *ts) {
    int y = 0, mo = 0, d = 0, hh = 0, mi = 0, ss = 0;
    if (sscanf(ts, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &hh, &mi, &ss) < 3) return NULL;
    struct tm utc;
    memset(&utc, 0, sizeof(utc));
    utc.tm_year = y - 1900; utc.tm_mon = mo - 1; utc.tm_mday = d;
    utc.tm_hour = hh; utc.tm_min = mi; utc.tm_sec = ss;
    time_t t = timegm(&utc);          // UTC として解釈
    struct tm local;
    localtime_r(&t, &local);          // ローカルへ変換
    char buf[16];
    strftime(buf, sizeof(buf), "%Y-%m-%d", &local);
    return g_strdup(buf);
}

// Claude Code がフォルダ名生成に使う規則: 英数字(ASCII)以外をすべて '-' に。
static char *encode_path(const char *s) {
    GString *out = g_string_sized_new(strlen(s));
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        gboolean alnum = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
        g_string_append_c(out, alnum ? (char)c : '-');
    }
    return g_string_free(out, FALSE);
}

// 末尾 '/' を除いた最後の要素を新規確保で返す。
static char *base_name_dup(const char *path) {
    gsize len = strlen(path);
    while (len > 0 && path[len - 1] == '/') len--;
    gsize start = 0;
    for (gsize i = 0; i < len; i++) if (path[i] == '/') start = i + 1;
    return g_strndup(path + start, len - start);
}

// home 配下なら接頭辞を ~ に短縮。新規確保で返す。
static char *display_path(Stats *s, const char *path) {
    const char *home = s->home_dir;
    if (home) {
        gsize hl = strlen(home);
        if (strcmp(path, home) == 0) return g_strdup("~");
        if (strncmp(path, home, hl) == 0 && path[hl] == '/')
            return g_strdup_printf("~%s", path + hl);
    }
    return g_strdup(path);
}

static gboolean dir_exists(const char *path) {
    return g_file_test(path, G_FILE_TEST_IS_DIR);
}

static const char *canonical_of(Stats *s, const char *folder) {
    const char *c = g_hash_table_lookup(s->by_project_canonical, folder);
    return c ? c : folder;
}

// フォルダの「真のルート cwd」を順序非依存に更新(encode 一致を優先、同条件は辞書順最小)。
static void update_canonical(Stats *s, const char *project, const char *cwd) {
    char *enc = encode_path(cwd);
    gboolean matches = (g_strcmp0(enc, project) == 0);
    g_free(enc);

    const char *cur = g_hash_table_lookup(s->by_project_canonical, project);
    if (!cur) {
        g_hash_table_insert(s->by_project_canonical, g_strdup(project), g_strdup(cwd));
        g_hash_table_insert(s->by_project_canonical_match, g_strdup(project), GINT_TO_POINTER(matches ? 1 : 0));
        return;
    }
    gboolean cur_match = GPOINTER_TO_INT(g_hash_table_lookup(s->by_project_canonical_match, project)) != 0;
    if (matches && !cur_match) {
        g_hash_table_insert(s->by_project_canonical, g_strdup(project), g_strdup(cwd));
        g_hash_table_insert(s->by_project_canonical_match, g_strdup(project), GINT_TO_POINTER(1));
    } else if (matches == cur_match && strcmp(cwd, cur) < 0) {
        g_hash_table_insert(s->by_project_canonical, g_strdup(project), g_strdup(cwd));
    }
}

// 1行(JSONL)を処理。集計に反映したら TRUE。
static gboolean process_line(Stats *s, const char *line, const char *project) {
    if (!line || !*line) return FALSE;
    JsonParser *parser = json_parser_new();
    GError *err = NULL;
    if (!json_parser_load_from_data(parser, line, -1, &err)) {
        if (err) g_error_free(err);
        g_object_unref(parser);
        return FALSE;
    }
    JsonNode *root_node = json_parser_get_root(parser);
    gboolean ret = FALSE;
    if (!root_node || !JSON_NODE_HOLDS_OBJECT(root_node)) goto done;
    JsonObject *root = json_node_get_object(root_node);

    // フォルダ識別用の cwd は、集計可否・dedup と独立に(全 early return より前で)捕捉する。
    const char *cwd = json_object_get_string_member_with_default(root, "cwd", NULL);
    if (cwd && *cwd) update_canonical(s, project, cwd);

    if (g_strcmp0(json_object_get_string_member_with_default(root, "type", ""), "assistant") != 0) goto done;
    if (!json_object_has_member(root, "message")) goto done;
    JsonNode *msg_node = json_object_get_member(root, "message");
    if (!JSON_NODE_HOLDS_OBJECT(msg_node)) goto done;
    JsonObject *msg = json_node_get_object(msg_node);

    if (!json_object_has_member(msg, "usage")) goto done;
    JsonNode *usage_node = json_object_get_member(msg, "usage");
    if (!JSON_NODE_HOLDS_OBJECT(usage_node)) goto done;
    JsonObject *usage = json_node_get_object(usage_node);

    const char *model = json_object_get_string_member_with_default(msg, "model", NULL);
    if (!model || !*model || g_strcmp0(model, "<synthetic>") == 0) goto done;

    // メッセージIDで重複排除
    if (json_object_has_member(msg, "id")) {
        const char *id = json_object_get_string_member_with_default(msg, "id", NULL);
        if (id && *id) {
            if (!g_hash_table_add(s->seen_ids, g_strdup(id))) goto done; // 既出
        }
    }

    gint64 input = get_int(usage, "input_tokens");
    gint64 output = get_int(usage, "output_tokens");
    gint64 cread = get_int(usage, "cache_read_input_tokens");
    gint64 ccreate_total = get_int(usage, "cache_creation_input_tokens");

    gint64 w5 = 0, w1 = 0;
    if (json_object_has_member(usage, "cache_creation") &&
        JSON_NODE_HOLDS_OBJECT(json_object_get_member(usage, "cache_creation"))) {
        JsonObject *cc = json_object_get_object_member(usage, "cache_creation");
        w5 = get_int(cc, "ephemeral_5m_input_tokens");
        w1 = get_int(cc, "ephemeral_1h_input_tokens");
    } else {
        w5 = ccreate_total;
    }

    double in_p, out_p;
    price_for(s, model, &in_p, &out_p);
    double cost =
        (double)input / 1e6 * in_p +
        (double)output / 1e6 * out_p +
        (double)w5 / 1e6 * in_p * CACHE_WRITE_5M_MULT +
        (double)w1 / 1e6 * in_p * CACHE_WRITE_1H_MULT +
        (double)cread / 1e6 * in_p * CACHE_READ_MULT;

    apply(&s->total, input, output, w5, w1, cread, cost);
    apply(get_bucket(s->by_model, model), input, output, w5, w1, cread, cost);
    apply(get_bucket(s->by_project, project), input, output, w5, w1, cread, cost);

    const char *ts = json_object_get_string_member_with_default(root, "timestamp", NULL);
    if (ts && *ts) {
        g_free(s->last_activity);
        s->last_activity = g_strdup(ts);
        char *day = local_day_key(ts);
        if (day) {
            apply(get_bucket(s->by_day, day), input, output, w5, w1, cread, cost);
            g_free(day);
        }
    }
    ret = TRUE;

done:
    g_object_unref(parser);
    return ret;
}

// 相対パスの先頭ディレクトリ名をプロジェクト名とする
static char *project_name_from(Stats *s, const char *path) {
    const char *rel = path;
    gsize plen = strlen(s->projects_dir);
    if (g_str_has_prefix(path, s->projects_dir)) {
        rel = path + plen;
        while (*rel == G_DIR_SEPARATOR) rel++;
    }
    const char *slash = strchr(rel, G_DIR_SEPARATOR);
    if (slash) return g_strndup(rel, slash - rel);
    return g_strdup(rel);
}

// 前回オフセット以降の追記分だけ読み込む
static void ingest_file(Stats *s, const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return;
    gint64 size = (gint64)st.st_size;

    gint64 *op = g_hash_table_lookup(s->offsets, path);
    gint64 offset = op ? *op : 0;
    if (size < offset) offset = 0;   // 縮んだ(再生成)ならリセット
    if (size == offset) return;      // 変化なし

    FILE *f = fopen(path, "rb");
    if (!f) return;
    if (fseek(f, (long)offset, SEEK_SET) != 0) { fclose(f); return; }
    gint64 len = size - offset;
    char *buf = g_malloc((gsize)len);
    gint64 nread = (gint64)fread(buf, 1, (gsize)len, f);
    fclose(f);
    if (nread <= 0) { g_free(buf); return; }

    // 完全な行(改行終端)までを処理。末尾の未完行は次回へ持ち越す。
    gint64 last_nl = -1;
    for (gint64 i = nread - 1; i >= 0; i--) { if (buf[i] == '\n') { last_nl = i; break; } }
    if (last_nl < 0) { g_free(buf); return; }

    char *project = project_name_from(s, path);
    // last_nl までを行分割（改行は含めない）
    gint64 start = 0;
    for (gint64 i = 0; i < last_nl; i++) {
        if (buf[i] == '\n') {
            char *line = g_strndup(buf + start, i - start);
            process_line(s, line, project);
            g_free(line);
            start = i + 1;
        }
    }
    if (start < last_nl) {
        char *line = g_strndup(buf + start, last_nl - start);
        process_line(s, line, project);
        g_free(line);
    }
    g_free(project);

    // 消費したバイト数 = last_nl(直前まで) + 改行1byte
    gint64 *np = g_new(gint64, 1);
    *np = offset + last_nl + 1;
    g_hash_table_insert(s->offsets, g_strdup(path), np);

    g_free(buf);
}

// *.jsonl を再帰列挙して ingest
static void scan_dir(Stats *s, const char *dir) {
    GDir *d = g_dir_open(dir, 0, NULL);
    if (!d) return;
    const char *name;
    while ((name = g_dir_read_name(d))) {
        char *full = g_build_filename(dir, name, NULL);
        if (g_file_test(full, G_FILE_TEST_IS_DIR)) {
            scan_dir(s, full);
        } else if (g_str_has_suffix(name, ".jsonl")) {
            ingest_file(s, full);
        }
        g_free(full);
    }
    g_dir_close(d);
}

void stats_full_scan(Stats *s) { scan_dir(s, s->projects_dir); }
void stats_tick(Stats *s) { scan_dir(s, s->projects_dir); }

// ---- スナップショット JSON ------------------------------------------------
static void add_bucket(JsonBuilder *b, const Bucket *bk, const char *name) {
    json_builder_begin_object(b);
    json_builder_set_member_name(b, "name");
    if (name) json_builder_add_string_value(b, name); else json_builder_add_null_value(b);
    json_builder_set_member_name(b, "input");        json_builder_add_int_value(b, bk->input);
    json_builder_set_member_name(b, "output");       json_builder_add_int_value(b, bk->output);
    json_builder_set_member_name(b, "cacheWrite5m"); json_builder_add_int_value(b, bk->cw5);
    json_builder_set_member_name(b, "cacheWrite1h"); json_builder_add_int_value(b, bk->cw1);
    json_builder_set_member_name(b, "cacheRead");    json_builder_add_int_value(b, bk->cread);
    json_builder_set_member_name(b, "cost");         json_builder_add_double_value(b, bk->cost);
    json_builder_set_member_name(b, "messages");     json_builder_add_int_value(b, bk->messages);
    json_builder_set_member_name(b, "tokens");       json_builder_add_int_value(b, bucket_tokens(bk));
    json_builder_end_object(b);
}

typedef struct { const char *name; Bucket *b; } Named;

static gint named_cmp_desc(gconstpointer a, gconstpointer b) {
    const Named *x = *(const Named * const *)a;
    const Named *y = *(const Named * const *)b;
    gint64 tx = bucket_tokens(x->b), ty = bucket_tokens(y->b);
    return (ty > tx) - (ty < tx); // tokens 降順
}

static void add_sorted_map(JsonBuilder *b, GHashTable *map) {
    GPtrArray *arr = g_ptr_array_new_with_free_func(g_free);
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, map);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        Named *n = g_new(Named, 1);
        n->name = (const char *)k;
        n->b = (Bucket *)v;
        g_ptr_array_add(arr, n);
    }
    g_ptr_array_sort(arr, named_cmp_desc);
    json_builder_begin_array(b);
    for (guint i = 0; i < arr->len; i++) {
        Named *n = g_ptr_array_index(arr, i);
        add_bucket(b, n->b, n->name);
    }
    json_builder_end_array(b);
    g_ptr_array_free(arr, TRUE);
}

// ---- プロジェクト統合(移動検知) ----------------------------------------
typedef struct { char *name; Bucket b; } Row;

static void bucket_add(Bucket *acc, const Bucket *bk) {
    acc->input += bk->input; acc->output += bk->output;
    acc->cw5 += bk->cw5; acc->cw1 += bk->cw1; acc->cread += bk->cread;
    acc->cost += bk->cost; acc->messages += bk->messages;
}

static gint row_cmp_desc(gconstpointer a, gconstpointer b) {
    const Row *x = *(const Row * const *)a;
    const Row *y = *(const Row * const *)b;
    gint64 tx = bucket_tokens(&x->b), ty = bucket_tokens(&y->b);
    return (ty > tx) - (ty < tx); // tokens 降順
}

static void row_free(gpointer p) { Row *r = p; g_free(r->name); g_free(r); }

// basename(真のルート) でグルーピングし、移動(存在1+消滅n)だけを1行へ統合する。
static void add_merged_projects(JsonBuilder *b, Stats *s) {
    // groups: basename(char*) -> GPtrArray<const char* folder>(借用)
    GHashTable *groups = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                               (GDestroyNotify)g_ptr_array_unref);
    GHashTableIter it; gpointer k, v;
    g_hash_table_iter_init(&it, s->by_project);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        const char *folder = k;
        char *base = base_name_dup(canonical_of(s, folder));
        GPtrArray *arr = g_hash_table_lookup(groups, base);
        if (!arr) { arr = g_ptr_array_new(); g_hash_table_insert(groups, base, arr); }
        else g_free(base);
        g_ptr_array_add(arr, (gpointer)folder);
    }

    GHashTable *exists = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GPtrArray *rows = g_ptr_array_new_with_free_func(row_free);

    g_hash_table_iter_init(&it, groups);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        GPtrArray *folders = v;
        GPtrArray *existing = g_ptr_array_new();
        guint gone = 0;
        for (guint i = 0; i < folders->len; i++) {
            const char *folder = g_ptr_array_index(folders, i);
            const char *can = canonical_of(s, folder);
            gpointer cached; gboolean ex;
            if (g_hash_table_lookup_extended(exists, can, NULL, &cached)) {
                ex = GPOINTER_TO_INT(cached) != 0;
            } else {
                ex = dir_exists(can);
                g_hash_table_insert(exists, g_strdup(can), GINT_TO_POINTER(ex ? 1 : 0));
            }
            if (ex) g_ptr_array_add(existing, (gpointer)folder); else gone++;
        }

        if (existing->len == 1 && gone >= 1) {
            const char *target = g_ptr_array_index(existing, 0);
            Row *r = g_new0(Row, 1);
            r->name = display_path(s, canonical_of(s, target));
            for (guint i = 0; i < folders->len; i++) {
                Bucket *bk = g_hash_table_lookup(s->by_project, g_ptr_array_index(folders, i));
                if (bk) bucket_add(&r->b, bk);
            }
            g_ptr_array_add(rows, r);
        } else {
            for (guint i = 0; i < folders->len; i++) {
                const char *folder = g_ptr_array_index(folders, i);
                Bucket *bk = g_hash_table_lookup(s->by_project, folder);
                Row *r = g_new0(Row, 1);
                r->name = display_path(s, canonical_of(s, folder));
                if (bk) r->b = *bk;
                g_ptr_array_add(rows, r);
            }
        }
        g_ptr_array_free(existing, TRUE);
    }

    g_ptr_array_sort(rows, row_cmp_desc);
    json_builder_begin_array(b);
    for (guint i = 0; i < rows->len; i++) {
        Row *r = g_ptr_array_index(rows, i);
        add_bucket(b, &r->b, r->name);
    }
    json_builder_end_array(b);

    g_ptr_array_free(rows, TRUE);
    g_hash_table_destroy(exists);
    g_hash_table_destroy(groups);
}

// 直近 days 日分の日別系列（0埋め、古い→新しい順）
static void add_daily(JsonBuilder *b, Stats *s, int days) {
    time_t now = time(NULL);
    struct tm base;
    localtime_r(&now, &base);
    base.tm_hour = 0; base.tm_min = 0; base.tm_sec = 0;

    json_builder_begin_array(b);
    for (int i = days - 1; i >= 0; i--) {
        struct tm t = base;
        t.tm_mday -= i;
        mktime(&t); // 正規化
        char key[16];
        strftime(key, sizeof(key), "%Y-%m-%d", &t);

        Bucket *bk = g_hash_table_lookup(s->by_day, key);
        json_builder_begin_object(b);
        json_builder_set_member_name(b, "date");     json_builder_add_string_value(b, key);
        json_builder_set_member_name(b, "tokens");   json_builder_add_int_value(b, bk ? bucket_tokens(bk) : 0);
        json_builder_set_member_name(b, "cost");     json_builder_add_double_value(b, bk ? bk->cost : 0.0);
        json_builder_set_member_name(b, "messages"); json_builder_add_int_value(b, bk ? bk->messages : 0);
        json_builder_end_object(b);
    }
    json_builder_end_array(b);
}

char *stats_snapshot_json(Stats *s) {
    JsonBuilder *b = json_builder_new();
    json_builder_begin_object(b);

    json_builder_set_member_name(b, "total");
    add_bucket(b, &s->total, NULL);

    json_builder_set_member_name(b, "models");
    add_sorted_map(b, s->by_model);

    json_builder_set_member_name(b, "projects");
    add_merged_projects(b, s);

    json_builder_set_member_name(b, "daily");
    add_daily(b, s, 371);

    json_builder_set_member_name(b, "lastActivity");
    if (s->last_activity) json_builder_add_string_value(b, s->last_activity);
    else json_builder_add_null_value(b);

    json_builder_set_member_name(b, "startedAt");  json_builder_add_string_value(b, s->started_at);
    json_builder_set_member_name(b, "projectsDir"); json_builder_add_string_value(b, s->projects_dir);
    json_builder_set_member_name(b, "intervalMs"); json_builder_add_int_value(b, s->interval_ms);
    json_builder_set_member_name(b, "pricing");    add_pricing_array(b, s); // UI のエディタ初期化用
    char *now = iso_now();
    json_builder_set_member_name(b, "now"); json_builder_add_string_value(b, now);
    g_free(now);

    json_builder_end_object(b);

    JsonGenerator *gen = json_generator_new();
    JsonNode *root = json_builder_get_root(b);
    json_generator_set_root(gen, root);
    char *out = json_generator_to_data(gen, NULL);

    json_node_free(root);
    g_object_unref(gen);
    g_object_unref(b);
    return out;
}
