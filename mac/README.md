# Claude Code トークン監視 — macOS 版 (.app)

Swift + AppKit + **WKWebView** によるネイティブ macOS アプリです。ローカルサーバーやポートを使わず、
WKWebView のウィンドウ内にダッシュボードを表示し、Swift 側で集計したデータを直接流し込みます。
集計ロジックは windows 版 `Stats.cs`（`server.js` 由来）を Swift へ移植したものです。

## ビルドと起動

```bash
cd mac
./build.sh          # dist/ClaudeTokenMonitor.app を生成
open dist/ClaudeTokenMonitor.app
```

`tokenmonitor` ランチャー（`~/.local/bin/tokenmonitor`）からも起動できます:

```bash
tokenmonitor          # 未ビルドなら build.sh 実行後に起動
tokenmonitor build    # 再ビルドしてから起動
```

## 必要環境

- macOS 11 以降（WKWebView 同梱、追加ランタイム不要）
- ビルドに Xcode Command Line Tools（`swiftc`）

## 仕組み

- 起動時に全 `~/.claude/projects/**/*.jsonl` をスキャンして集計。以後は N秒間隔タイマーで
  追記分のみ読み込み（ファイルごとのバイトオフセット管理）。
- メッセージID重複排除、`<synthetic>` モデル除外、サブエージェント分も合算。
- 集計結果は `WKWebView.evaluateJavaScript("window.__recv(<json>)")` でダッシュボードへ push。
- ページ → ネイティブは `window.webkit.messageHandlers.tokenmonitor.postMessage()` を使用
  （`"ready"` で初回要求、`{type:"interval",ms}` で更新間隔変更）。
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
| `Sources/main.swift` | AppKit ウィンドウ + WKWebView のホスト・配線 |
| `Sources/Stats.swift` | JSONL 解析・トークン/コスト集計・ファイル監視 |
| `web/index.html` | ダッシュボード UI（WebKit メッセージで受信） |
| `web/chart.umd.min.js` | グラフ描画（ローカル同梱） |
| `build.sh` | `.app` バンドルのビルドスクリプト |
