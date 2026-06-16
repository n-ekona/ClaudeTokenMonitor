# Claude Code トークン監視

このフォルダには2つの版があります。用途で使い分けてください。

## web/ — ブラウザ版（Node / 依存パッケージ不要）
ブラウザでダッシュボードを表示。パスワード認証つき。

- `web\start-browser.bat` … このPCで起動（自動で `http://localhost:4317` を開く）
- `web\start-browser-tunnel.bat` … 外出先公開（Cloudflare Tunnel で一時URLを発行）
- `web\export-web.bat` … **サーバー不要の静的版**を書き出す。暗号化された `index.html` を1枚、任意のHTTPSホスト（例: `nekona.jp/claudetoken/`）に置くだけで、パスワード入力→閲覧できる。

詳細は [web/README.md](web/README.md)。

## desktop/ — アプリ版（.exe / C# + WebView2）
ローカルサーバーもポートも使わないネイティブ Windows アプリ。時系列グラフ・草（ヒートマップ）入り。

- `desktop\publish\ClaudeTokenMonitor.exe` をダブルクリックで起動

詳細は [desktop/README.md](desktop/README.md)。
