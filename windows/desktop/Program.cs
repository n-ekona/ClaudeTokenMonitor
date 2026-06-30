using System;
using System.IO;
using System.Windows;
using System.Windows.Media.Imaging;
using Microsoft.Web.WebView2.Core;
using Microsoft.Web.WebView2.Wpf;

namespace TokenMonitor;

internal static class Program
{
    // フチなしPiPウィンドウのドラッグ移動用（OSのウィンドウ移動ループを起動する）
    private const int WM_NCLBUTTONDOWN = 0xA1;
    private const int HTCAPTION = 0x2;

    [System.Runtime.InteropServices.DllImport("user32.dll")]
    private static extern bool ReleaseCapture();

    [System.Runtime.InteropServices.DllImport("user32.dll")]
    private static extern IntPtr SendMessage(IntPtr hWnd, int msg, IntPtr wParam, IntPtr lParam);

    // タイトルバーをダーク/ライト表示にする（Windows 10 2004+ / 11）
    private const int DWMWA_USE_IMMERSIVE_DARK_MODE = 20;

    [System.Runtime.InteropServices.DllImport("dwmapi.dll")]
    private static extern int DwmSetWindowAttribute(IntPtr hwnd, int attr, ref int value, int size);

    // PiP ウィンドウの不透明度をレイヤードウィンドウで実現する（WPF の Opacity は
    // AllowsTransparency=true が必要で実行時に切替できないため、Win32 で直接行う）。
    private const int GWL_EXSTYLE = -20;
    private const int WS_EX_LAYERED = 0x00080000;
    private const uint LWA_ALPHA = 0x2;

    [System.Runtime.InteropServices.DllImport("user32.dll", SetLastError = true)]
    private static extern int GetWindowLong(IntPtr hWnd, int nIndex);

    [System.Runtime.InteropServices.DllImport("user32.dll", SetLastError = true)]
    private static extern int SetWindowLong(IntPtr hWnd, int nIndex, int dwNewLong);

    [System.Runtime.InteropServices.DllImport("user32.dll", SetLastError = true)]
    private static extern bool SetLayeredWindowAttributes(IntPtr hwnd, uint crKey, byte bAlpha, uint dwFlags);

    [STAThread]
    private static void Main()
    {
        var app = new Application { ShutdownMode = ShutdownMode.OnMainWindowClose };

        var win = new Window
        {
            Title = "Claude Code トークン監視",
            Width = 1240,
            Height = 860,
            WindowStartupLocation = WindowStartupLocation.CenterScreen,
            Background = System.Windows.Media.Brushes.Black,
        };

        // ウィンドウ/タスクバーのアイコン（出力フォルダの icon.ico）
        try
        {
            var iconPath = Path.Combine(AppContext.BaseDirectory, "icon.ico");
            if (File.Exists(iconPath)) win.Icon = BitmapFrame.Create(new Uri(iconPath));
        }
        catch { /* アイコン無し/読込失敗でも起動は続行 */ }

        // WebView2 初期化中にウィンドウを閉じると native 側でデッドロック（ハング）するため、
        // 初期化完了まで WM_CLOSE を握りつぶして「閉じる操作」自体を無効化する。
        // ウィンドウ自体は即表示するので起動は速い（初期化中に閉じても無視されるだけ）。
        var coreReady = false;
        win.SourceInitialized += (_, _) =>
        {
            var src = (System.Windows.Interop.HwndSource)System.Windows.PresentationSource.FromVisual(win)!;
            src.AddHook((IntPtr hwnd, int msg, IntPtr w, IntPtr l, ref bool handled) =>
            {
                const int WM_CLOSE = 0x0010;
                if (msg == WM_CLOSE && !coreReady) handled = true; // 初期化完了まで閉じない
                return IntPtr.Zero;
            });
        };

        var web = new WebView2();
        win.Content = web;

        var stats = new Stats();

        // 閉じる際はタイマーを止める（終了時の Dispatcher シャットダウンと Timer コールバックの競合を防ぐ）
        win.Closing += (_, _) => stats.Stop();

        win.Loaded += async (_, _) =>
        {
            // WebView2 のユーザーデータは LocalAppData 配下に置く
            var dataDir = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
                "ClaudeTokenMonitor");
            Directory.CreateDirectory(dataDir);

            try
            {
                var env = await CoreWebView2Environment.CreateAsync(null, dataDir);
                await web.EnsureCoreWebView2Async(env);
                coreReady = true; // コア初期化完了。危険区間を抜けたので以降は閉じてよい
            }
            catch (Exception ex)
            {
                coreReady = true; // 初期化失敗時もクローズ保留を解除して終了できるようにする
                MessageBox.Show(
                    "WebView2 ランタイムの初期化に失敗しました。\n" +
                    "Microsoft Edge WebView2 ランタイムをインストールしてください。\n\n" + ex.Message,
                    "起動エラー", MessageBoxButton.OK, MessageBoxImage.Error);
                app.Shutdown();
                return;
            }

            var core = web.CoreWebView2;

            // web フォルダを仮想ホストにマッピング（ローカルファイル配信）
            var webDir = Path.Combine(AppContext.BaseDirectory, "web");
            core.SetVirtualHostNameToFolderMapping(
                "tokenmonitor.local", webDir, CoreWebView2HostResourceAccessKind.Allow);

            // 開発者ツールやコンテキストメニューは無効化（任意）
            core.Settings.AreDefaultContextMenusEnabled = false;
            core.Settings.IsStatusBarEnabled = false;

            // 正方形(PiP)モードの状態。通常時のウィンドウ位置/サイズを保存して復元する。
            var pipOn = false;
            double prevW = win.Width, prevH = win.Height, prevL = win.Left, prevT = win.Top;
            var prevState = win.WindowState;
            var prevResize = win.ResizeMode;
            var prevStyle = win.WindowStyle;

            // タイトルバー（とウィンドウ素地）の配色をテーマに合わせる
            void ApplyTheme(bool dark)
            {
                var hwnd = new System.Windows.Interop.WindowInteropHelper(win).Handle;
                if (hwnd != IntPtr.Zero)
                {
                    int v = dark ? 1 : 0;
                    try { DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, ref v, sizeof(int)); }
                    catch { /* 古い Windows では未対応・無視 */ }
                }
                win.Background = dark
                    ? System.Windows.Media.Brushes.Black
                    : System.Windows.Media.Brushes.White;
            }

            // PiP ウィンドウの不透明度を適用/解除（レイヤードウィンドウ）。
            void ApplyPipOpacity(int opacity)
            {
                var hwnd = new System.Windows.Interop.WindowInteropHelper(win).Handle;
                if (hwnd == IntPtr.Zero) return;
                try
                {
                    int ex = GetWindowLong(hwnd, GWL_EXSTYLE);
                    SetWindowLong(hwnd, GWL_EXSTYLE, ex | WS_EX_LAYERED);
                    byte alpha = (byte)Math.Clamp(opacity * 255 / 100, 0, 255);
                    SetLayeredWindowAttributes(hwnd, 0, alpha, LWA_ALPHA);
                }
                catch { /* 古いOS/失敗時は不透明のまま */ }
            }
            void RemovePipOpacity()
            {
                var hwnd = new System.Windows.Interop.WindowInteropHelper(win).Handle;
                if (hwnd == IntPtr.Zero) return;
                try
                {
                    int ex = GetWindowLong(hwnd, GWL_EXSTYLE);
                    if ((ex & WS_EX_LAYERED) != 0)
                        SetWindowLong(hwnd, GWL_EXSTYLE, ex & ~WS_EX_LAYERED);
                }
                catch { /* 失敗は無視 */ }
            }

            void SetPip(bool on, double size, int opacity)
            {
                if (size < 240) size = 240;
                if (size > 520) size = 520;
                if (opacity < 30) opacity = 30;
                if (opacity > 100) opacity = 100;
                if (on && !pipOn)
                {
                    // 通常表示の復元用に現在のジオメトリ/スタイルを退避
                    prevW = win.Width; prevH = win.Height; prevL = win.Left; prevT = win.Top;
                    prevState = win.WindowState; prevResize = win.ResizeMode; prevStyle = win.WindowStyle;

                    win.WindowState = WindowState.Normal;
                    // タイトルバー（最小化/最大化/×）と枠を消してフチなしの正方形にする
                    win.WindowStyle = WindowStyle.None;
                    win.ResizeMode = ResizeMode.NoResize;
                    win.Topmost = true;                 // 常に最前面
                    win.Width = size; win.Height = size;
                    // 画面右下のタスクバーを避けた作業領域の隅へ配置（PiP風）
                    var wa = SystemParameters.WorkArea;
                    win.Left = wa.Right - size - 24;
                    win.Top = wa.Bottom - size - 24;
                    pipOn = true;
                }
                else if (on && pipOn)
                {
                    // 既に PiP 表示中にサイズだけ変わった場合は、その場でリサイズして配置し直す
                    win.Width = size; win.Height = size;
                    var wa = SystemParameters.WorkArea;
                    win.Left = wa.Right - size - 24;
                    win.Top = wa.Bottom - size - 24;
                }
                else if (!on && pipOn)
                {
                    win.Topmost = false;
                    win.WindowStyle = prevStyle;
                    win.ResizeMode = prevResize;
                    win.WindowState = prevState;
                    win.Width = prevW; win.Height = prevH;
                    win.Left = prevL; win.Top = prevT;
                    pipOn = false;
                }

                // PiP 中だけ不透明度を適用し、通常表示へ戻したら解除する。
                if (pipOn) ApplyPipOpacity(opacity);
                else RemovePipOpacity();
            }

            // ページからのメッセージ処理:
            //   "ready"                       → 初回スナップショット送信
            //   {"type":"interval","ms":N}    → 更新間隔を変更
            //   {"type":"pip","on":b,"size":N}→ 正方形(PiP)モードの切替
            //   {"type":"drag"}               → PiP ウィンドウのドラッグ開始
            //   {"type":"projectsDir","path"} → 監視対象パスの変更＋再スキャン
            //   {"type":"rescan"}             → 集計を全クリアして再スキャン
            //   {"type":"pricing","table":[]} → モデル単価の上書き＋再計算
            //   {"type":"theme","mode":"dark"}→ タイトルバー配色の切替
            core.WebMessageReceived += (_, e) =>
            {
                string? msg = null;
                try { msg = e.TryGetWebMessageAsString(); } catch { /* JSON以外/null */ }

                if (!string.IsNullOrEmpty(msg) && msg.TrimStart().StartsWith("{", StringComparison.Ordinal))
                {
                    try
                    {
                        using var doc = System.Text.Json.JsonDocument.Parse(msg);
                        var root = doc.RootElement;
                        var type = root.TryGetProperty("type", out var t) ? t.GetString() : null;
                        if (type == "interval"
                            && root.TryGetProperty("ms", out var m) && m.TryGetInt32(out var ms))
                        {
                            stats.SetInterval(ms);
                        }
                        else if (type == "pip" && root.TryGetProperty("on", out var o))
                        {
                            double size = root.TryGetProperty("size", out var sz) && sz.TryGetDouble(out var szv) ? szv : 320;
                            int opacity = root.TryGetProperty("opacity", out var op) && op.TryGetInt32(out var opv) ? opv : 100;
                            SetPip(o.ValueKind == System.Text.Json.JsonValueKind.True, size, opacity);
                        }
                        else if (type == "projectsDir")
                        {
                            var path = root.TryGetProperty("path", out var p) ? p.GetString() : null;
                            stats.SetProjectsDir(path);
                        }
                        else if (type == "rescan")
                        {
                            stats.Rescan();
                        }
                        else if (type == "pricing" && root.TryGetProperty("table", out var tbl)
                                 && tbl.ValueKind == System.Text.Json.JsonValueKind.Array)
                        {
                            stats.SetPricingFromJson(tbl);
                        }
                        else if (type == "theme")
                        {
                            var mode = root.TryGetProperty("mode", out var md) ? md.GetString() : null;
                            ApplyTheme(mode != "light"); // 既定はダーク
                        }
                        else if (type == "drag" && pipOn)
                        {
                            // フチなしPiPでもウィンドウを掴んで移動できるようにする。
                            // WebView2 上ではマウス入力が WebView2 の HWND に渡るため WPF の
                            // win.DragMove() は「左ボタン押下中」と判定できず効かない。
                            // OS のウィンドウ移動ループを直接起動するため WM_NCLBUTTONDOWN を送る。
                            var hwnd = new System.Windows.Interop.WindowInteropHelper(win).Handle;
                            if (hwnd != IntPtr.Zero)
                            {
                                ReleaseCapture();
                                SendMessage(hwnd, WM_NCLBUTTONDOWN, (IntPtr)HTCAPTION, IntPtr.Zero);
                            }
                        }
                    }
                    catch { /* 不正なメッセージは無視 */ }
                }
                Push();
            };
            core.NavigationCompleted += (_, _) => Push();

            // 集計更新を UI スレッドへマーシャリングして配信。
            // 終了処理中(Dispatcherシャットダウン後)にタイマーが発火しても落ちないようにガードする。
            stats.Changed += () =>
            {
                if (win.Dispatcher.HasShutdownStarted || win.Dispatcher.HasShutdownFinished) return;
                try { win.Dispatcher.BeginInvoke(new Action(Push)); }
                catch (InvalidOperationException) { /* Dispatcher シャットダウン中 */ }
            };

            core.Navigate("https://tokenmonitor.local/index.html");
            stats.Start();

            void Push()
            {
                try { web.CoreWebView2?.PostWebMessageAsJson(stats.SnapshotJson()); }
                catch { /* ナビゲーション中などは無視 */ }
            }
        };

        app.Run(win);
    }
}
