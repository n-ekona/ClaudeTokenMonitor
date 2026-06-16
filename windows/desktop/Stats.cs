using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;
using System.Text.Json;
using System.Threading;

namespace TokenMonitor;

// Claude Code のトランスクリプト(~/.claude/projects/**/*.jsonl)を集計する。
// server.js のロジックを C# へ移植したもの。
public sealed class Stats
{
    // 料金表 (per MTok: input, output)
    private static readonly (string key, double inP, double outP)[] Pricing =
    {
        ("claude-fable-5", 10, 50),
        ("claude-mythos-5", 10, 50),
        ("claude-opus-4-8", 5, 25),
        ("claude-opus-4-7", 5, 25),
        ("claude-opus-4-6", 5, 25),
        ("claude-opus-4-5", 5, 25),
        ("claude-opus-4-1", 15, 75),
        ("claude-opus-4-0", 15, 75),
        ("claude-sonnet-4-6", 3, 15),
        ("claude-sonnet-4-5", 3, 15),
        ("claude-sonnet-4-0", 3, 15),
        ("claude-haiku-4-5", 1, 5),
        ("claude-3-5-haiku", 0.8, 4),
        ("claude-3-haiku", 0.25, 1.25),
    };
    private const double CacheWrite5mMult = 1.25;
    private const double CacheWrite1hMult = 2.0;
    private const double CacheReadMult = 0.1;

    private static readonly JsonSerializerOptions JsonOpts = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
    };

    public string ProjectsDir { get; }
    public event Action? Changed;

    private readonly object _lock = new();
    private readonly Bucket _total = new();
    private readonly Dictionary<string, Bucket> _byModel = new();
    private readonly Dictionary<string, Bucket> _byProject = new();
    private readonly Dictionary<string, Bucket> _byDay = new(); // "yyyy-MM-dd"(ローカル日付) -> bucket
    private readonly HashSet<string> _seenIds = new();
    private readonly Dictionary<string, long> _offsets = new();
    private string? _lastActivity;
    private readonly string _startedAt = DateTime.UtcNow.ToString("o");

    private Timer? _interval;
    private int _intervalMs;

    public Stats()
    {
        ProjectsDir = Environment.GetEnvironmentVariable("CLAUDE_PROJECTS_DIR")
            ?? Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
                ".claude", "projects");

        var ms = Environment.GetEnvironmentVariable("REFRESH_INTERVAL_MS");
        _intervalMs = int.TryParse(ms, out var v) ? Math.Clamp(v, 500, 600000) : 3000;
    }

    private static (double inP, double outP) PriceFor(string model)
    {
        foreach (var p in Pricing)
            if (model == p.key || model.StartsWith(p.key, StringComparison.Ordinal))
                return (p.inP, p.outP);
        return (5, 25); // フォールバック
    }

    public void Start()
    {
        FullScan();
        // N秒ごとに追記分を読み込み、毎ティック通知する（UIはこの値へ向けてカウントアップ）
        _interval = new Timer(_ => Tick(), null, _intervalMs, _intervalMs);
    }

    // 更新間隔をミリ秒で変更（UI/環境変数から）
    public void SetInterval(int ms)
    {
        _intervalMs = Math.Clamp(ms, 500, 600000);
        _interval?.Change(_intervalMs, _intervalMs);
    }

    // タイマーを停止（ウィンドウ終了時に呼ぶ）。終了後の不要なファイル走査と
    // Dispatcher シャットダウン競合を防ぐ。
    public void Stop()
    {
        _interval?.Dispose();
        _interval = null;
    }

    private int _ticking;

    private void Tick()
    {
        // タイマーコールバックの再入を防止（読込が間隔より長引いた場合に重ならないように）
        if (Interlocked.Exchange(ref _ticking, 1) == 1) return;
        try
        {
            lock (_lock)
            {
                foreach (var f in EnumerateJsonl())
                    IngestFile(f);
            }
            // 変化の有無に関わらず毎ティック通知（UIのハートビート＆相対時刻更新のため）
            Changed?.Invoke();
        }
        finally
        {
            Interlocked.Exchange(ref _ticking, 0);
        }
    }

    private IEnumerable<string> EnumerateJsonl()
    {
        if (!Directory.Exists(ProjectsDir)) yield break;
        IEnumerable<string> files;
        try { files = Directory.EnumerateFiles(ProjectsDir, "*.jsonl", SearchOption.AllDirectories); }
        catch { yield break; }
        foreach (var f in files) yield return f;
    }

    private void FullScan()
    {
        lock (_lock)
        {
            foreach (var f in EnumerateJsonl()) IngestFile(f);
        }
    }

    // 前回オフセット以降の追記分だけ読み込む。反映があれば true。
    private bool IngestFile(string filePath)
    {
        long size;
        try
        {
            var info = new FileInfo(filePath);
            if (!info.Exists) return false;
            size = info.Length;
        }
        catch { return false; }

        long offset = _offsets.TryGetValue(filePath, out var o) ? o : 0;
        if (size < offset) offset = 0;       // 縮んだ(再生成)ならリセット
        if (size == offset) return false;     // 変化なし

        byte[] buf;
        try
        {
            using var fs = new FileStream(filePath, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
            fs.Seek(offset, SeekOrigin.Begin);
            int len = (int)(size - offset);
            buf = new byte[len];
            int read = 0;
            while (read < len)
            {
                int n = fs.Read(buf, read, len - read);
                if (n <= 0) break;
                read += n;
            }
            if (read < len) Array.Resize(ref buf, read);
        }
        catch { return false; }

        // 完全な行(改行終端)までを処理。末尾の未完行は次回へ持ち越す。
        int lastNl = Array.LastIndexOf(buf, (byte)0x0a);
        if (lastNl == -1) return false;

        string text = Encoding.UTF8.GetString(buf, 0, lastNl);
        string project = ProjectNameFromFile(filePath);
        bool changed = false;
        foreach (var line in text.Split('\n'))
            if (ProcessLine(line, project)) changed = true;

        _offsets[filePath] = offset + Encoding.UTF8.GetByteCount(text) + 1;
        return changed;
    }

    private string ProjectNameFromFile(string filePath)
    {
        var rel = Path.GetRelativePath(ProjectsDir, filePath);
        var idx = rel.IndexOfAny(new[] { Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar });
        return idx >= 0 ? rel[..idx] : rel;
    }

    private bool ProcessLine(string line, string project)
    {
        if (string.IsNullOrWhiteSpace(line)) return false;
        JsonDocument doc;
        try { doc = JsonDocument.Parse(line); }
        catch { return false; }

        using (doc)
        {
            var root = doc.RootElement;
            if (root.ValueKind != JsonValueKind.Object) return false;
            if (!root.TryGetProperty("type", out var typeEl) || typeEl.GetString() != "assistant") return false;
            if (!root.TryGetProperty("message", out var msg) || msg.ValueKind != JsonValueKind.Object) return false;
            if (!msg.TryGetProperty("usage", out var usage) || usage.ValueKind != JsonValueKind.Object) return false;
            if (!msg.TryGetProperty("model", out var modelEl)) return false;
            var model = modelEl.GetString();
            if (string.IsNullOrEmpty(model) || model == "<synthetic>") return false;

            // メッセージIDで重複排除
            if (msg.TryGetProperty("id", out var idEl) && idEl.ValueKind == JsonValueKind.String)
            {
                var id = idEl.GetString()!;
                if (!_seenIds.Add(id)) return false;
            }

            long input = GetLong(usage, "input_tokens");
            long output = GetLong(usage, "output_tokens");
            long cacheRead = GetLong(usage, "cache_read_input_tokens");
            long cacheCreateTotal = GetLong(usage, "cache_creation_input_tokens");

            long w5 = 0, w1 = 0;
            if (usage.TryGetProperty("cache_creation", out var cc) && cc.ValueKind == JsonValueKind.Object)
            {
                w5 = GetLong(cc, "ephemeral_5m_input_tokens");
                w1 = GetLong(cc, "ephemeral_1h_input_tokens");
            }
            else
            {
                w5 = cacheCreateTotal;
            }

            var (inP, outP) = PriceFor(model);
            double cost =
                input / 1e6 * inP +
                output / 1e6 * outP +
                w5 / 1e6 * inP * CacheWrite5mMult +
                w1 / 1e6 * inP * CacheWrite1hMult +
                cacheRead / 1e6 * inP * CacheReadMult;

            Apply(_total, input, output, w5, w1, cacheRead, cost);
            Apply(GetBucket(_byModel, model), input, output, w5, w1, cacheRead, cost);
            Apply(GetBucket(_byProject, project), input, output, w5, w1, cacheRead, cost);

            if (root.TryGetProperty("timestamp", out var tsEl) && tsEl.ValueKind == JsonValueKind.String)
            {
                var ts = tsEl.GetString();
                _lastActivity = ts;
                // タイムスタンプをローカル日付に変換して日別バケットへ加算
                if (DateTimeOffset.TryParse(ts, System.Globalization.CultureInfo.InvariantCulture,
                        System.Globalization.DateTimeStyles.AssumeUniversal, out var tsOff))
                {
                    var dayKey = tsOff.ToLocalTime().ToString("yyyy-MM-dd");
                    Apply(GetBucket(_byDay, dayKey), input, output, w5, w1, cacheRead, cost);
                }
            }

            return true;
        }
    }

    private static long GetLong(JsonElement obj, string name)
        => obj.TryGetProperty(name, out var el) && el.ValueKind == JsonValueKind.Number && el.TryGetInt64(out var v) ? v : 0;

    private static Bucket GetBucket(Dictionary<string, Bucket> map, string key)
    {
        if (!map.TryGetValue(key, out var b)) { b = new Bucket(); map[key] = b; }
        return b;
    }

    private static void Apply(Bucket b, long input, long output, long w5, long w1, long cacheRead, double cost)
    {
        b.Input += input;
        b.Output += output;
        b.CacheWrite5m += w5;
        b.CacheWrite1h += w1;
        b.CacheRead += cacheRead;
        b.Cost += cost;
        b.Messages += 1;
    }

    // index.html が期待する形の JSON 文字列を返す
    public string SnapshotJson()
    {
        lock (_lock)
        {
            var models = _byModel
                .Select(kv => kv.Value.ToDto(kv.Key))
                .OrderByDescending(d => d.Tokens)
                .ToList();
            var projects = _byProject
                .Select(kv => kv.Value.ToDto(kv.Key))
                .OrderByDescending(d => d.Tokens)
                .ToList();

            var snap = new
            {
                total = _total.ToDto(null),
                models,
                projects,
                daily = DailySeries(371), // 時系列グラフ & 草(ヒートマップ)用: 直近53週
                lastActivity = _lastActivity,
                startedAt = _startedAt,
                projectsDir = ProjectsDir,
                intervalMs = _intervalMs,
                now = DateTime.UtcNow.ToString("o"),
            };
            return JsonSerializer.Serialize(snap, JsonOpts);
        }
    }

    // 直近 days 日分の日別系列（データの無い日も0埋め、古い→新しい順）。呼び出しは _lock 内で。
    private List<DailyDto> DailySeries(int days)
    {
        var list = new List<DailyDto>(days);
        var today = DateTime.Now.Date;
        for (int i = days - 1; i >= 0; i--)
        {
            var key = today.AddDays(-i).ToString("yyyy-MM-dd");
            if (_byDay.TryGetValue(key, out var b))
                list.Add(new DailyDto
                {
                    Date = key,
                    Tokens = b.Input + b.Output + b.CacheWrite5m + b.CacheWrite1h + b.CacheRead,
                    Cost = b.Cost,
                    Messages = b.Messages,
                });
            else
                list.Add(new DailyDto { Date = key, Tokens = 0, Cost = 0, Messages = 0 });
        }
        return list;
    }

    private sealed class DailyDto
    {
        public string Date { get; set; } = "";
        public long Tokens { get; set; }
        public double Cost { get; set; }
        public long Messages { get; set; }
    }

    private sealed class Bucket
    {
        public long Input, Output, CacheWrite5m, CacheWrite1h, CacheRead, Messages;
        public double Cost;

        public BucketDto ToDto(string? name) => new()
        {
            Name = name,
            Input = Input,
            Output = Output,
            CacheWrite5m = CacheWrite5m,
            CacheWrite1h = CacheWrite1h,
            CacheRead = CacheRead,
            Cost = Cost,
            Messages = Messages,
            Tokens = Input + Output + CacheWrite5m + CacheWrite1h + CacheRead,
        };
    }

    private sealed class BucketDto
    {
        public string? Name { get; set; }
        public long Input { get; set; }
        public long Output { get; set; }
        public long CacheWrite5m { get; set; }
        public long CacheWrite1h { get; set; }
        public long CacheRead { get; set; }
        public double Cost { get; set; }
        public long Messages { get; set; }
        public long Tokens { get; set; }
    }
}
