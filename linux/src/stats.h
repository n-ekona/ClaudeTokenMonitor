#ifndef TOKENMONITOR_STATS_H
#define TOKENMONITOR_STATS_H

#include <glib.h>
#include <json-glib/json-glib.h>

// Claude Code のトランスクリプト(~/.claude/projects/**/*.jsonl)を集計する。
// windows 版 Stats.cs (server.js からの移植) を C へ移植したもの。

typedef struct _Stats Stats;

Stats      *stats_new(void);
void        stats_free(Stats *s);

// 起動時の全スキャン
void        stats_full_scan(Stats *s);
// 追記分のみ読み込み（タイマーから毎回呼ぶ）
void        stats_tick(Stats *s);

// index.html が期待する形の JSON 文字列を返す（呼び出し側で g_free）
char       *stats_snapshot_json(Stats *s);

void        stats_set_interval(Stats *s, int ms);
int         stats_interval_ms(Stats *s);
const char *stats_projects_dir(Stats *s);

// 監視対象パスを変更し、全クリアして再スキャン。空/NULL なら既定へ戻す。
// 集計更新と config 保存のみ行う（UI 通知は呼び出し側で push する）。
void        stats_set_projects_dir(Stats *s, const char *path);
// 集計を全クリアして再スキャン（リセット）。
void        stats_rescan(Stats *s);
// モデル単価を上書きし、コスト再計算のため全クリアして再スキャン＋config 保存。
// 空/NULL の JsonArray なら既定14件へ戻す。
void        stats_set_pricing(Stats *s, JsonArray *table);

// UI 設定（不透明な JSON オブジェクト）を保存する。集計に影響しないため再スキャンしない。
// settings の所有権は移さない（内部で複製して保持する）。
void        stats_set_ui(Stats *s, JsonNode *settings);

#endif // TOKENMONITOR_STATS_H
