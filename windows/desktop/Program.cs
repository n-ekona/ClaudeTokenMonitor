using System;
using System.IO;
using System.Windows;
using Microsoft.Web.WebView2.Core;
using Microsoft.Web.WebView2.Wpf;

namespace TokenMonitor;

internal static class Program
{
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

            // ページからのメッセージ処理:
            //   "ready"                      → 初回スナップショット送信
            //   {"type":"interval","ms":N}   → 更新間隔を変更
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
                        if (root.TryGetProperty("type", out var t) && t.GetString() == "interval"
                            && root.TryGetProperty("ms", out var m) && m.TryGetInt32(out var ms))
                        {
                            stats.SetInterval(ms);
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
