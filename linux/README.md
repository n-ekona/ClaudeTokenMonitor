# Claude Code トークン監視 — Linux 版

C + GTK3 + **WebKitGTK** によるネイティブ Linux アプリです。ローカルサーバーやポートを使わず、
WebKitWebView のウィンドウ内にダッシュボードを表示し、C 側で集計したデータを直接流し込みます。
集計ロジックは windows 版 `Stats.cs`（`server.js` 由来）を C へ移植したものです。

## 依存パッケージ

| ディストリ | インストール |
|---|---|
| Debian/Ubuntu | `sudo apt install build-essential libgtk-3-dev libwebkit2gtk-4.1-dev libjson-glib-dev` |
| Fedora | `sudo dnf install gcc gtk3-devel webkit2gtk4.1-devel json-glib-devel` |
| Arch | `sudo pacman -S base-devel gtk3 webkit2gtk-4.1 json-glib` |

`webkit2gtk-4.1` が無い環境では `webkit2gtk-4.0` に自動フォールバックします（2.40 以降が必要）。

## ビルドと起動

```bash
cd linux
./build.sh          # または make
./tokenmonitor
```

## 仕組み

- 起動時に全 `~/.claude/projects/**/*.jsonl` をスキャンして集計。以後は N秒間隔タイマー
  （GLib `g_timeout_add`）で追記分のみ読み込み（ファイルごとのバイトオフセット管理）。
- メッセージID重複排除、`<synthetic>` モデル除外、サブエージェント分も合算。
- 集計結果は `webkit_web_view_evaluate_javascript("window.__recv(<json>)")` でダッシュボードへ push。
- ページ → ネイティブは `window.webkit.messageHandlers.tokenmonitor.postMessage()` を使用
  （`"ready"` で初回要求、`{type:"interval",ms}` で更新間隔変更 → タイマー再スケジュール）。
- 更新間隔は画面右上から 1〜600 秒で変更可能。環境変数 `REFRESH_INTERVAL_MS` で初期値指定可。
- 為替（USD/JPY）は画面側で無料APIから自動取得（30分ごと）。ネットワークに出るのはこの為替取得のみ。

## 設定（環境変数）

| 変数 | 既定値 | 説明 |
|---|---|---|
| `CLAUDE_PROJECTS_DIR` | `~/.claude/projects` | 走査するトランスクリプトのディレクトリ |
| `REFRESH_INTERVAL_MS` | `3000` | 更新間隔(ミリ秒)の初期値。画面からも変更可 |

## 構成ファイル

| ファイル | 役割 |
|---|---|
| `src/main.c` | GTK ウィンドウ + WebKitWebView のホスト・配線 |
| `src/stats.c` / `src/stats.h` | JSONL 解析・トークン/コスト集計・ファイル監視 |
| `web/index.html` | ダッシュボード UI（WebKit メッセージで受信） |
| `web/chart.umd.min.js` | グラフ描画（ローカル同梱） |
| `build.sh` / `Makefile` | ビルドスクリプト |
