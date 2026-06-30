# Claude Code トークン監視 / Claude Token Monitor

![Version](https://img.shields.io/github/v/release/n-ekona/ClaudeTokenMonitor)
![License](https://img.shields.io/github/license/n-ekona/ClaudeTokenMonitor)
![Platform](https://img.shields.io/badge/platform-Windows%20%7C%20macOS%20%7C%20Linux-blue)
![C#](https://img.shields.io/badge/C%23-512BD4?logo=csharp&logoColor=white)
![Swift](https://img.shields.io/badge/Swift-F05138?logo=swift&logoColor=white)
![C](https://img.shields.io/badge/C-A8B9CC?logo=c&logoColor=black)
![JavaScript](https://img.shields.io/badge/JavaScript-F7DF1E?logo=javascript&logoColor=black)

`~/.claude/projects/**/*.jsonl` を解析して、Claude Code のトークン使用量・コストをリアルタイムに可視化するツールです。時系列グラフと GitHub 風のアクティビティ・ヒートマップ付き。完全ローカル動作（為替取得を除く）。Windows・macOS・Linux のネイティブアプリを用意しています。

## 主な機能

- トークン使用量・コスト・メッセージ数の集計と、モデル別・プロジェクト別の内訳表示。
- 日別の時系列グラフと、GitHub 風のアクティビティ・ヒートマップ。
- 設定パネルから、監視対象パス・更新間隔・モデル単価・表示通貨・テーマを変更できる。設定は各 OS の設定ファイルへ保存する。
- テーマはダーク/ライト/システム追従に加え、15種類のプリセットとカスタム配色を用意。タイトルバーやグラフの色まで連動する。
- 表示通貨は日本円ほか14通貨から選べ、為替レートを自動取得する。
- 11言語のインターフェース（日本語・英語・中国語・韓国語ほか）。ブラウザの言語から自動で選ぶ。
- PiP（正方形）モード。常に最前面の小さなウィンドウに主要な数値を表示する。3 OS に対応し、ウィンドウの不透明度も調整できる。

## ダウンロード

最新版は [**Releases**](../../releases/latest) から入手できます。

### Windows（.NET 9 + WebView2）

- `*-win-x64-selfcontained.zip` … **おすすめ。** .NET ランタイム同梱。展開して `ClaudeTokenMonitor.exe` をダブルクリックするだけ。
- `*-win-x64.zip` … 軽量版。別途 [.NET 9 ランタイム](https://dotnet.microsoft.com/download/dotnet/9.0) が必要。

> どちらも Microsoft Edge WebView2 ランタイムが必要です（Windows 11 は標準搭載）。

### macOS（Swift + WKWebView）

- `*-mac-universal.zip` … Intel / Apple Silicon 両対応のユニバーサルバイナリ。展開して `ClaudeTokenMonitor.app` を実行。追加ランタイムは不要（macOS 11 以降）。

### Linux（C + GTK3 + WebKitGTK）

ディストリごとにビルドした版を用意しています。

- `*-linux-ubuntu-x86_64.zip` … Ubuntu 24.04 / Debian 系
- `*-linux-almalinux9-x86_64.zip` … AlmaLinux 9 / RHEL 系
- `*-linux-archlinux-x86_64.zip` … Arch Linux 系

> 実行には GTK3 / WebKitGTK 4.1 / json-glib のランタイムが必要です（例: Ubuntu は `sudo apt install libgtk-3-0 libwebkit2gtk-4.1-0 libjson-glib-1.0-0`）。

## ソースからビルド

```bash
cd windows/desktop

# 開発実行
dotnet run -c Release

# 配布用 exe を作る
dotnet publish -c Release -r win-x64 --self-contained false -o publish
```

詳細は [windows/desktop/README.md](windows/desktop/README.md) を参照してください。

### macOS（Swift + WKWebView）

```bash
cd mac
./build.sh          # dist/ClaudeTokenMonitor.app を生成
open dist/ClaudeTokenMonitor.app
```

詳細は [mac/README.md](mac/README.md) を参照してください。

### Linux（C + GTK3 + WebKitGTK）

```bash
cd linux
./build.sh          # または make
./tokenmonitor
```

詳細は [linux/README.md](linux/README.md) を参照してください。

> 各 OS 版は同じダッシュボード UI（`web/index.html`）を、OS ネイティブの WebView
> （Windows=WebView2 / macOS=WKWebView / Linux=WebKitGTK）でネイティブ表示します。
> 集計ロジックは各 OS のネイティブ言語（C# / Swift / C）に移植して共通の仕様で動作します。

## リリースの作り方（メンテナ向け）

バージョンタグを push すると、[`.github/workflows/release.yml`](.github/workflows/release.yml) が
自動でビルド・zip 化し、GitHub Release に添付します。

```bash
git tag v1.0.0
git push origin v1.0.0
```

`Actions` タブの **Release** ワークフローを手動実行（workflow_dispatch）して
バージョンを指定することもできます（その場合は Release は作られず成果物のみ）。

## ライセンス

[MIT](LICENSE) © nekoNA
