# Claude Code トークン監視 / Claude Token Monitor

`~/.claude/projects/**/*.jsonl` を解析して、Claude Code のトークン使用量・コストをリアルタイムに可視化するツールです。時系列グラフと GitHub 風の「草」（アクティビティ・ヒートマップ）付き。完全ローカル動作（為替取得を除く）。

## ダウンロード（Windows ネイティブアプリ）

最新版は [**Releases**](../../releases/latest) から入手できます。

- `*-selfcontained.zip` … **おすすめ。** .NET ランタイム同梱。展開して `ClaudeTokenMonitor.exe` をダブルクリックするだけ。
- `*-win-x64.zip` … 軽量版。別途 [.NET 9 ランタイム](https://dotnet.microsoft.com/download/dotnet/9.0) が必要。

> どちらも Microsoft Edge WebView2 ランタイムが必要です（Windows 11 は標準搭載）。

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
