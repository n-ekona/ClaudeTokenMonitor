#ifndef TOKENMONITOR_STATS_H
#define TOKENMONITOR_STATS_H

#include <glib.h>

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

#endif // TOKENMONITOR_STATS_H
