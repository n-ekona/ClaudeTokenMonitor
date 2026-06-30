import Cocoa
import WebKit

// Claude Code トークン監視 — ネイティブ macOS アプリ (AppKit + WKWebView)。
// ローカルサーバーもポートも使わず、WKWebView 内にダッシュボードを表示し、
// Stats が集計した値を evaluateJavaScript で直接流し込む。

final class Bridge: NSObject, WKScriptMessageHandler, WKNavigationDelegate, WKUIDelegate {
    let stats: Stats
    weak var webView: WKWebView?
    // ウィンドウ操作は AppDelegate 側へ委譲（onChanged と同じパターン）。実体はメインスレッドで呼ぶ。
    var onPip: ((Bool, Double, Int) -> Void)?
    var onDrag: (() -> Void)?
    var onTheme: ((Bool) -> Void)?

    init(stats: Stats) {
        self.stats = stats
        super.init()
    }

    // ページ → ネイティブ:
    //   "ready"                        → 初回スナップショット送信
    //   {"type":"interval","ms":N}     → 更新間隔を変更＋保存
    //   {"type":"projectsDir","path":s}→ 監視対象パスの変更＋再スキャン
    //   {"type":"rescan"}              → 集計を全クリアして再スキャン
    //   {"type":"pricing","table":[]}  → モデル単価の上書き＋再計算
    //   {"type":"theme","mode":"dark"} → ウィンドウ配色の切替
    //   {"type":"pip","on":b,"size":N} → 正方形(PiP)モードの切替
    //   {"type":"drag"}                → PiP ウィンドウのドラッグ開始
    func userContentController(_ ucc: WKUserContentController, didReceive message: WKScriptMessage) {
        if let s = message.body as? String,
           s.trimmingCharacters(in: .whitespaces).hasPrefix("{"),
           let data = s.data(using: .utf8),
           let obj = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] {
            switch obj["type"] as? String {
            case "interval":
                if let ms = (obj["ms"] as? NSNumber)?.intValue { stats.setInterval(ms) }
            case "projectsDir":
                stats.setProjectsDir(obj["path"] as? String)
            case "rescan":
                stats.rescan()
            case "pricing":
                if let table = obj["table"] as? [Any] { stats.setPricingFromJson(table) }
            case "ui":
                if let settings = obj["settings"] as? [String: Any] { stats.setUi(settings) }
            case "theme":
                let dark = (obj["mode"] as? String) != "light" // 既定はダーク
                let cb = onTheme
                DispatchQueue.main.async { cb?(dark) }
            case "pip":
                let on = (obj["on"] as? NSNumber)?.boolValue ?? false
                let size = (obj["size"] as? NSNumber)?.doubleValue ?? 320
                let opacity = (obj["opacity"] as? NSNumber)?.intValue ?? 100
                let cb = onPip
                DispatchQueue.main.async { cb?(on, size, opacity) }
            case "drag":
                // ドラッグは現在のマウスイベントを使うため main で同期実行する
                // （async だと NSApp.currentEvent が失われ performDrag を呼べない）。
                // 本ハンドラは WKScriptMessageHandler 仕様で既に main スレッド上で呼ばれる。
                onDrag?()
            default:
                break
            }
        }
        push()
    }

    func webView(_ webView: WKWebView, didFinish navigation: WKNavigation!) {
        push()
    }

    // file:// 以外への遷移を遮断する（ローカル完結アプリのため外部ナビゲーションは不要）。
    // fetch()/XHR はナビゲーションではないため本判定の対象外で、為替取得には影響しない。
    func webView(_ webView: WKWebView,
                 decidePolicyFor navigationAction: WKNavigationAction,
                 decisionHandler: @escaping (WKNavigationActionPolicy) -> Void) {
        if let url = navigationAction.request.url, !(url.isFileURL || url.scheme == "about") {
            decisionHandler(.cancel)
        } else {
            decisionHandler(.allow)
        }
    }

    // window.open / target=_blank による新規ウィンドウ生成を拒否する。
    func webView(_ webView: WKWebView,
                 createWebViewWith configuration: WKWebViewConfiguration,
                 for navigationAction: WKNavigationAction,
                 windowFeatures: WKWindowFeatures) -> WKWebView? {
        return nil
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

// borderless(.borderless)にすると既定では canBecomeKey/Main が false になり、
// PiP 中にキー入力・初回クリックが届かないことがある。常に key/main になれるよう上書き。
final class MainWindow: NSWindow {
    override var canBecomeKey: Bool { true }
    override var canBecomeMain: Bool { true }
}

final class AppDelegate: NSObject, NSApplicationDelegate, NSWindowDelegate {
    var window: NSWindow!
    var webView: WKWebView!
    let stats = Stats()
    var bridge: Bridge!

    // PiP（正方形）モードの退避状態。通常時の frame/styleMask/level/movable を保存して復元する。
    private var pipOn = false
    private var savedFrame = NSRect.zero
    private var savedStyleMask: NSWindow.StyleMask = []
    private var savedLevel: NSWindow.Level = .normal
    private var savedMovableByBg = false
    private var savedAlpha: CGFloat = 1.0

    func applicationDidFinishLaunching(_ notification: Notification) {
        bridge = Bridge(stats: stats)

        let config = WKWebViewConfiguration()
        let ucc = WKUserContentController()
        ucc.add(bridge, name: "tokenmonitor")
        config.userContentController = ucc

        let frame = NSRect(x: 0, y: 0, width: 1240, height: 860)
        webView = WKWebView(frame: frame, configuration: config)
        webView.navigationDelegate = bridge
        webView.uiDelegate = bridge                        // window.open を遮断する
        webView.setValue(false, forKey: "drawsBackground") // 黒背景の透けを防ぐ
        bridge.webView = webView

        window = MainWindow(
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

        // ウィンドウ操作（テーマ/PiP/ドラッグ）を Bridge から委譲してもらう
        bridge.onTheme = { [weak self] dark in self?.applyTheme(dark) }
        bridge.onPip = { [weak self] on, size, opacity in self?.setPip(on, size, opacity) }
        bridge.onDrag = { [weak self] in self?.pipDrag() }

        // web フォルダの index.html を読み込む（バンドル Resources、無ければ実行ファイル隣）
        if let webDir = Self.locateWebDir() {
            let index = webDir.appendingPathComponent("index.html")
            webView.loadFileURL(index, allowingReadAccessTo: webDir)
        }

        stats.start()
    }

    // ネイティブ枠の配色をテーマに合わせる。appearance でタイトルバー/スクロールバーが追従。
    private func applyTheme(_ dark: Bool) {
        window.appearance = NSAppearance(named: dark ? .darkAqua : .aqua)
        window.backgroundColor = dark ? .black : .white
    }

    // 正方形(PiP)モードの切替（Windows の SetPip と同等）。
    private func setPip(_ on: Bool, _ size: Double, _ opacity: Int) {
        var s = size
        if s < 240 { s = 240 }
        if s > 520 { s = 520 }
        let side = CGFloat(s)
        let alpha = CGFloat(min(max(opacity, 30), 100)) / 100.0

        if on && !pipOn {
            // 通常表示の復元用に現在のジオメトリ/スタイル/不透明度を退避
            savedFrame = window.frame
            savedStyleMask = window.styleMask
            savedLevel = window.level
            savedMovableByBg = window.isMovableByWindowBackground
            savedAlpha = window.alphaValue

            window.styleMask = .borderless            // タイトルバー/枠なしの正方形
            window.level = .floating                  // 常に最前面
            window.isMovableByWindowBackground = true // 背景ドラッグで移動可能
            window.setFrame(pipFrame(side), display: true)
            window.makeKeyAndOrderFront(nil)
            pipOn = true
        } else if on && pipOn {
            // 既に PiP 表示中にサイズだけ変わった場合は、その場でリサイズして配置し直す
            window.setFrame(pipFrame(side), display: true)
        } else if !on && pipOn {
            // 退避したスタイル/レベル/ジオメトリ/可動性/不透明度を復元
            window.styleMask = savedStyleMask
            window.level = savedLevel
            window.isMovableByWindowBackground = savedMovableByBg
            window.setFrame(savedFrame, display: true)
            window.alphaValue = savedAlpha
            window.makeKeyAndOrderFront(nil)
            pipOn = false
        }

        // PiP 中だけ不透明度を反映（通常表示へ戻したら savedAlpha で復元済み）。
        if pipOn { window.alphaValue = alpha }
    }

    // visibleFrame（メニューバー/Dock を除く作業領域）の右下隅へ、24pt のマージンで配置する正方形。
    // macOS は原点が左下なので「右下」は x=maxX-side-margin, y=minY+margin。
    private func pipFrame(_ side: CGFloat) -> NSRect {
        let margin: CGFloat = 24
        let vf = (window.screen ?? NSScreen.main)?.visibleFrame
            ?? NSRect(x: 0, y: 0, width: side + margin * 2, height: side + margin * 2)
        return NSRect(x: vf.maxX - side - margin, y: vf.minY + margin, width: side, height: side)
    }

    // PiP のドラッグ。borderless + isMovableByWindowBackground=true により背景ドラッグでも
    // 動くが、JS の pointerdown 起点で即ドラッグを開始できるよう、現在のマウスイベントが
    // 取れる場合は performDrag を呼ぶ。取れない場合は背景ドラッグに委ねる（安全側）。
    private func pipDrag() {
        guard pipOn else { return }
        if let ev = NSApp.currentEvent,
           ev.type == .leftMouseDown || ev.type == .leftMouseDragged {
            window.performDrag(with: ev)
        }
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
