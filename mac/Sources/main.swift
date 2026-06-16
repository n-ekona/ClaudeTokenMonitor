import Cocoa
import WebKit

// Claude Code トークン監視 — ネイティブ macOS アプリ (AppKit + WKWebView)。
// ローカルサーバーもポートも使わず、WKWebView 内にダッシュボードを表示し、
// Stats が集計した値を evaluateJavaScript で直接流し込む。

final class Bridge: NSObject, WKScriptMessageHandler, WKNavigationDelegate {
    let stats: Stats
    weak var webView: WKWebView?

    init(stats: Stats) {
        self.stats = stats
        super.init()
    }

    // ページ → ネイティブ:
    //   "ready"                      → 初回スナップショット送信
    //   {"type":"interval","ms":N}   → 更新間隔を変更
    func userContentController(_ ucc: WKUserContentController, didReceive message: WKScriptMessage) {
        if let s = message.body as? String,
           s.trimmingCharacters(in: .whitespaces).hasPrefix("{"),
           let data = s.data(using: .utf8),
           let obj = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
           (obj["type"] as? String) == "interval",
           let ms = (obj["ms"] as? NSNumber)?.intValue {
            stats.setInterval(ms)
        }
        push()
    }

    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
        push()
    }

    // 集計スナップショットを UI スレッドからページへ配信。
    // JSON はそのまま JS の式（オブジェクトリテラル）として評価できる。
    func push() {
        let json = stats.snapshotJson()
        DispatchQueue.main.async { [weak self] in
            self?.webView?.evaluateJavaScript("window.__recv(\(json))", completionHandler: nil)
        }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate, NSWindowDelegate {
    var window: NSWindow!
    var webView: WKWebView!
    let stats = Stats()
    var bridge: Bridge!

    func applicationDidFinishLaunching(_ notification: Notification) {
        bridge = Bridge(stats: stats)

        let config = WKWebViewConfiguration()
        let ucc = WKUserContentController()
        ucc.add(bridge, name: "tokenmonitor")
        config.userContentController = ucc

        let frame = NSRect(x: 0, y: 0, width: 1240, height: 860)
        webView = WKWebView(frame: frame, configuration: config)
        webView.navigationDelegate = bridge
        webView.setValue(false, forKey: "drawsBackground") // 黒背景の透けを防ぐ
        bridge.webView = webView

        window = NSWindow(
            contentRect: frame,
            styleMask: [.titled, .closable, .miniaturizable, .resizable],
            backing: .buffered,
            defer: false)
        window.title = "Claude Code トークン監視"
        window.backgroundColor = .black
        window.delegate = self
        window.contentView = webView
        window.center()
        window.makeKeyAndOrderFront(nil)

        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)

        // 集計更新をUIスレッドへマーシャリングして配信
        stats.onChanged = { [weak self] in self?.bridge.push() }

        // web フォルダの index.html を読み込む（バンドル Resources、無ければ実行ファイル隣）
        if let webDir = Self.locateWebDir() {
            let index = webDir.appendingPathComponent("index.html")
            webView.loadFileURL(index, allowingReadAccessTo: webDir)
        }

        stats.start()
    }

    func windowWillClose(_ notification: Notification) {
        stats.stop()
        NSApp.terminate(nil)
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        return true
    }

    // .app バンドルの Contents/Resources/web、開発時は実行ファイルと同階層の web を探す
    static func locateWebDir() -> URL? {
        if let res = Bundle.main.resourceURL {
            let w = res.appendingPathComponent("web")
            if FileManager.default.fileExists(atPath: w.appendingPathComponent("index.html").path) { return w }
        }
        let exeDir = URL(fileURLWithPath: CommandLine.arguments[0]).deletingLastPathComponent()
        let w = exeDir.appendingPathComponent("web")
        if FileManager.default.fileExists(atPath: w.appendingPathComponent("index.html").path) { return w }
        return nil
    }
}

// アプリ起動
let app = NSApplication.shared
let delegate = AppDelegate()
app.delegate = delegate

// 最小限のメニュー（Cmd+Q で終了できるように）
let mainMenu = NSMenu()
let appMenuItem = NSMenuItem()
mainMenu.addItem(appMenuItem)
let appMenu = NSMenu()
appMenu.addItem(withTitle: "Claude Code トークン監視を終了",
                action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
appMenuItem.submenu = appMenu
app.mainMenu = mainMenu

app.run()
