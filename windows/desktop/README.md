# Claude Code トークン監視 — デスクトップ版 (.exe)

.NET 9 + WebView2 によるネイティブ Windows アプリです。ローカルサーバーやポートを使わず、
WebView2 のウィンドウ内にダッシュボードを表示し、C# 側で集計したデータを直接流し込みます。
完全オフラインで動作します（Chart.js も同梱）。

## できあがった exe

```
desktop\publish\ClaudeTokenMonitor.exe   ← これをダブルクリックで起動
desktop\publish\web\                      ← exe と同じ場所に必要（UI/グラフ）
```

`publish` フォルダごとコピーすれば他の Windows PC でも動きます
（.NET 9 ランタイムと Microsoft Edge WebView2 ランタイムが必要。WebView2 は Windows 11 に標準搭載）。

## ビルド方法

```bash
cd desktop

# 開発ビルド & 実行
dotnet run -c Release

# 配布用 exe を作る（フレームワーク依存・軽量）
dotnet publish -c Release -r win-x64 --self-contained false -o publish
```

### .NET ランタイム不要の単体 exe が欲しい場合（サイズ大）

```bash
dotnet publish -c Release -r win-x64 --self-contained true -o publish-standalone
```

→ `publish-standalone\ClaudeTokenMonitor.exe`（.NET ランタイム同梱、約 150MB）。
WebView2 ランタイムだけは別途必要（Windows 11 は標準搭載）。

## リアルタイム更新とカウントアップ

- バックエンド(`Stats.cs`)は**N秒ごと**（既定3秒）に `~/.claude/projects/**/*.jsonl` の**追記分だけ**を
  読み込み、`WebView2.PostWebMessageAsJson()` でダッシュボードへスナップショットを push する。
- フロント(`web\index.html`)は受け取った実値へ向けて `requestAnimationFrame` で数字を
  **滑らかにカウントアップ**（初回は0から、以降は前値からイージング）。増加時はハイライト＆ステータスをパルス表示。
- **更新間隔は画面右上のセレクタから変更可能**（1〜600秒）。変更は `{type:'interval',ms}` メッセージで
  バックエンドへ即反映される。環境変数 `REFRESH_INTERVAL_MS` でも初期値を指定可。

## 時系列グラフ & 草（アクティビティ・ヒートマップ）

- 「モデル別／プロジェクト別」ウィジェットの上に、**日別の使用量**（トークン棒＋コスト折れ線・7/14/30日切替）と、
  GitHub風の**草**（直近約1年の日別アクティビティ）を表示します。
- 集計は `Stats.cs` が各メッセージの `timestamp` を**ローカル日付**でバケット化し、スナップショットに
  直近 **371日分**の `daily`（日付・トークン・コスト・メッセージ）を載せて配信します。
- 草の濃淡は、その期間の非ゼロ日のトークン量を四分位で5段階に色分け。セルにカーソルを置くと日付・内訳をツールチップ表示。
- ⚠ この2機能はバックエンド(C#)の集計追加を含むため、**反映には .exe の再ビルドが必要**です
  （`dotnet publish -c Release -r win-x64 --self-contained false -o publish`）。本リポジトリの `publish` は更新済み。

## PiP（正方形）モード

- ヘッダーの「**⛶ PiP**」ボタンで、フチなし（タイトルバー・枠なし）の小さな**正方形ウィンドウ**に切り替わります。
  **常に最前面**に固定され、ウィンドウ内を**ドラッグして移動**できます（右上の「⤢」で通常表示へ戻る）。
- 表示は、左上に**直近7日のアクティビティ（草）**、背景に**モデル別の円グラフ＋プロジェクト別の帯グラフ**、
  中央に**総トークン（実数・カンマ区切り）と省略形（〜M）**、**推定コスト（USD/円）**。
- ウィンドウ制御は C# 側（`Program.cs`）で行うため、`{type:'pip'|'drag'}` メッセージで連携します。

## 円相場（USD/JPY）の自動取得

- 画面右上の「**自動**」チェック時、起動時と**30分ごと**に無料の為替APIから USD/JPY を取得し、
  コストの円換算へ即反映します（`open.er-api.com` → 失敗時 `exchangerate-api.com` の順。APIキー不要・CORS対応）。
- レート欄を**手入力**すると自動更新は解除され、入力値が使われます（「自動」を再チェックで復帰）。
- 取得値は端末に保存し、次回起動時やオフライン時は**前回値**を使用（取得できなければ既定 155 円）。
- ネットワークに出るのはこの為替取得のみで、トークン集計自体は従来どおりローカル完結です。
- 反映方法: `web\index.html` の差し替えだけ。**.exe の再ビルドは不要**（次回起動時に読み込まれます）。

## 仕組み

- 起動時に全 `.jsonl` をスキャンして集計。以後は N秒間隔タイマーで追記分のみ読み込み（オフセット管理）。
- タイマーは再入防止（`Interlocked`）。終了時はタイマーを停止し、Dispatcher シャットダウンとの競合を回避。
- `web\` フォルダは `SetVirtualHostNameToFolderMapping` で仮想ホスト配信（`https://tokenmonitor.local/`）。
- メッセージID重複排除、`<synthetic>` モデル除外、サブエージェント分も合算。
- WebView2 のコア初期化中（起動〜約1〜2秒）は閉じる操作を無効化し、初期化途中の破棄による
  native デッドロックを回避（初期化完了後は通常どおり閉じられる）。

## 設定（環境変数）

| 変数 | 既定値 | 説明 |
|---|---|---|
| `CLAUDE_PROJECTS_DIR` | `~/.claude/projects` | 走査するトランスクリプトのディレクトリ |
| `REFRESH_INTERVAL_MS` | `3000` | 更新間隔(ミリ秒)の初期値。画面からも変更可 |

## 構成ファイル

| ファイル | 役割 |
|---|---|
| `Program.cs` | WPF ウィンドウ + WebView2 のホスト・配線 |
| `Stats.cs` | JSONL 解析・トークン/コスト集計・ファイル監視 |
| `web\index.html` | ダッシュボード UI（WebView2 メッセージで受信） |
| `web\chart.umd.min.js` | グラフ描画（ローカル同梱） |
| `TokenMonitor.csproj` | プロジェクト定義 |
| `app.manifest` | DPI 対応設定 |
