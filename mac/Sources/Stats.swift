import Foundation

// Claude Code のトランスクリプト(~/.claude/projects/**/*.jsonl)を集計する。
// windows 版 Stats.cs (server.js からの移植) を Swift へ移植したもの。
final class Stats {
    // 料金表 (per MTok: input, output)
    private static let pricing: [(key: String, inP: Double, outP: Double)] = [
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
    ]
    private static let cacheWrite5mMult = 1.25
    private static let cacheWrite1hMult = 2.0
    private static let cacheReadMult = 0.1

    struct Bucket {
        var input = 0, output = 0, cacheWrite5m = 0, cacheWrite1h = 0, cacheRead = 0, messages = 0
        var cost = 0.0
        var tokens: Int { input + output + cacheWrite5m + cacheWrite1h + cacheRead }
    }

    let projectsDir: String
    private let homeDir: String                    // displayPath の ~ 短縮基準(projectsDir 上書きと無関係に実 home)
    var onChanged: (() -> Void)?

    private let lock = NSLock()
    private var total = Bucket()
    private var byModel: [String: Bucket] = [:]
    private var byProject: [String: Bucket] = [:]
    private var byDay: [String: Bucket] = [:]      // "yyyy-MM-dd"(ローカル日付) -> bucket
    // フォルダ(プロジェクトキー) -> 真のルート cwd と、それが encode(cwd)==folder か。移動統合用。
    private var byProjectCanonical: [String: String] = [:]
    private var byProjectCanonicalMatch: [String: Bool] = [:]
    private var seenIds = Set<String>()
    private var offsets: [String: Int] = [:]
    private var lastActivity: String?
    private let startedAt: String

    private var timer: DispatchSourceTimer?
    private let timerQueue = DispatchQueue(label: "tokenmonitor.stats")
    private var intervalMs: Int
    private var ticking = false

    private let isoOut: ISO8601DateFormatter
    private let isoParse: ISO8601DateFormatter
    private let dayFmt: DateFormatter

    init() {
        let env = ProcessInfo.processInfo.environment
        homeDir = FileManager.default.homeDirectoryForCurrentUser.path
        if let dir = env["CLAUDE_PROJECTS_DIR"], !dir.isEmpty {
            projectsDir = dir
        } else {
            projectsDir = (homeDir as NSString).appendingPathComponent(".claude/projects")
        }
        if let ms = env["REFRESH_INTERVAL_MS"], let v = Int(ms) {
            intervalMs = min(max(v, 500), 600000)
        } else {
            intervalMs = 3000
        }

        isoOut = ISO8601DateFormatter()
        isoOut.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        isoParse = ISO8601DateFormatter()
        isoParse.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        dayFmt = DateFormatter()
        dayFmt.locale = Locale(identifier: "en_US_POSIX")
        dayFmt.dateFormat = "yyyy-MM-dd"
        dayFmt.timeZone = TimeZone.current

        startedAt = isoOut.string(from: Date())
    }

    private static func priceFor(_ model: String) -> (Double, Double) {
        for p in pricing where model == p.key || model.hasPrefix(p.key) {
            return (p.inP, p.outP)
        }
        return (5, 25) // フォールバック
    }

    func start() {
        fullScan()
        let t = DispatchSource.makeTimerSource(queue: timerQueue)
        t.schedule(deadline: .now() + .milliseconds(intervalMs),
                   repeating: .milliseconds(intervalMs))
        t.setEventHandler { [weak self] in self?.tick() }
        timer = t
        t.resume()
    }

    // 更新間隔をミリ秒で変更（UI/環境変数から）
    func setInterval(_ ms: Int) {
        let clamped = min(max(ms, 500), 600000)
        timerQueue.async { [weak self] in
            guard let self = self else { return }
            self.intervalMs = clamped
            self.timer?.schedule(deadline: .now() + .milliseconds(clamped),
                                 repeating: .milliseconds(clamped))
        }
    }

    // タイマーを停止（ウィンドウ終了時に呼ぶ）。
    func stop() {
        timer?.cancel()
        timer = nil
    }

    private func tick() {
        // タイマーコールバックの再入を防止
        if ticking { return }
        ticking = true
        defer { ticking = false }
        lock.lock()
        for f in enumerateJsonl() { _ = ingestFile(f) }
        lock.unlock()
        // 変化の有無に関わらず毎ティック通知（UIのハートビート＆相対時刻更新のため）
        onChanged?()
    }

    private func enumerateJsonl() -> [String] {
        let fm = FileManager.default
        var isDir: ObjCBool = false
        guard fm.fileExists(atPath: projectsDir, isDirectory: &isDir), isDir.boolValue else { return [] }
        guard let en = fm.enumerator(atPath: projectsDir) else { return [] }
        var out: [String] = []
        for case let rel as String in en where rel.hasSuffix(".jsonl") {
            out.append((projectsDir as NSString).appendingPathComponent(rel))
        }
        return out
    }

    private func fullScan() {
        lock.lock()
        for f in enumerateJsonl() { _ = ingestFile(f) }
        lock.unlock()
    }

    // 前回オフセット以降の追記分だけ読み込む。反映があれば true。
    @discardableResult
    private func ingestFile(_ filePath: String) -> Bool {
        let fm = FileManager.default
        guard let attrs = try? fm.attributesOfItem(atPath: filePath),
              let sizeNum = attrs[.size] as? NSNumber else { return false }
        let size = sizeNum.intValue

        var offset = offsets[filePath] ?? 0
        if size < offset { offset = 0 }   // 縮んだ(再生成)ならリセット
        if size == offset { return false } // 変化なし

        guard let fh = FileHandle(forReadingAtPath: filePath) else { return false }
        defer { try? fh.close() }
        var data: Data
        do {
            try fh.seek(toOffset: UInt64(offset))
            data = fh.readDataToEndOfFile()
        } catch { return false }

        // 完全な行(改行終端)までを処理。末尾の未完行は次回へ持ち越す。
        guard let lastNl = data.lastIndex(of: 0x0a) else { return false }
        let complete = data.subdata(in: data.startIndex..<lastNl) // 末尾改行は含めない
        guard let text = String(data: complete, encoding: .utf8) else {
            // 不正バイトがあっても消費は進める（無限ループ回避）
            offsets[filePath] = offset + (lastNl - data.startIndex) + 1
            return false
        }
        let project = projectName(from: filePath)
        var changed = false
        for line in text.split(separator: "\n", omittingEmptySubsequences: false) {
            if processLine(String(line), project: project) { changed = true }
        }
        // 消費した完全行のバイト数 + 改行1byte
        offsets[filePath] = offset + text.utf8.count + 1
        return changed
    }

    private func projectName(from filePath: String) -> String {
        var rel = filePath
        if rel.hasPrefix(projectsDir) {
            rel = String(rel.dropFirst(projectsDir.count))
            while rel.hasPrefix("/") { rel.removeFirst() }
        }
        if let slash = rel.firstIndex(of: "/") {
            return String(rel[rel.startIndex..<slash])
        }
        return rel
    }

    // Claude Code がフォルダ名生成に使う規則: 英数字(ASCII)以外をすべて '-' に。
    private static func encodePath(_ s: String) -> String {
        var out = ""
        out.reserveCapacity(s.utf8.count)
        for u in s.unicodeScalars {
            let v = u.value
            let isAlnum = (v >= 48 && v <= 57) || (v >= 65 && v <= 90) || (v >= 97 && v <= 122)
            out.append(isAlnum ? Character(u) : "-")
        }
        return out
    }

    private static func baseName(_ path: String) -> String {
        var p = Substring(path)
        while p.hasSuffix("/") { p = p.dropLast() }
        if let i = p.lastIndex(of: "/") { return String(p[p.index(after: i)...]) }
        return String(p)
    }

    // home 配下なら接頭辞を ~ に短縮。
    private func displayPath(_ path: String) -> String {
        if path == homeDir { return "~" }
        if path.hasPrefix(homeDir + "/") { return "~" + path.dropFirst(homeDir.count) }
        return path
    }

    // フォルダの「真のルート cwd」を順序非依存に更新(encode 一致を優先、同条件は辞書順最小)。
    private func updateCanonical(project: String, cwd: String) {
        let matches = (Stats.encodePath(cwd) == project)
        guard let cur = byProjectCanonical[project] else {
            byProjectCanonical[project] = cwd
            byProjectCanonicalMatch[project] = matches
            return
        }
        let curMatch = byProjectCanonicalMatch[project] ?? false
        if matches && !curMatch {
            byProjectCanonical[project] = cwd
            byProjectCanonicalMatch[project] = true
        } else if matches == curMatch && cwd < cur {
            byProjectCanonical[project] = cwd
        }
    }

    private func processLine(_ line: String, project: String) -> Bool {
        let trimmed = line.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmed.isEmpty { return false }
        guard let lineData = line.data(using: .utf8),
              let root = (try? JSONSerialization.jsonObject(with: lineData)) as? [String: Any]
        else { return false }

        // フォルダ識別用の cwd は、集計可否・dedup と独立に(全 early return より前で)捕捉する。
        if let cwd = root["cwd"] as? String, !cwd.isEmpty {
            updateCanonical(project: project, cwd: cwd)
        }

        guard (root["type"] as? String) == "assistant" else { return false }
        guard let msg = root["message"] as? [String: Any] else { return false }
        guard let usage = msg["usage"] as? [String: Any] else { return false }
        guard let model = msg["model"] as? String, !model.isEmpty, model != "<synthetic>" else { return false }

        // メッセージIDで重複排除
        if let id = msg["id"] as? String {
            if !seenIds.insert(id).inserted { return false }
        }

        let input = getInt(usage, "input_tokens")
        let output = getInt(usage, "output_tokens")
        let cacheRead = getInt(usage, "cache_read_input_tokens")
        let cacheCreateTotal = getInt(usage, "cache_creation_input_tokens")

        var w5 = 0, w1 = 0
        if let cc = usage["cache_creation"] as? [String: Any] {
            w5 = getInt(cc, "ephemeral_5m_input_tokens")
            w1 = getInt(cc, "ephemeral_1h_input_tokens")
        } else {
            w5 = cacheCreateTotal
        }

        let (inP, outP) = Stats.priceFor(model)
        let cost =
            Double(input) / 1e6 * inP +
            Double(output) / 1e6 * outP +
            Double(w5) / 1e6 * inP * Stats.cacheWrite5mMult +
            Double(w1) / 1e6 * inP * Stats.cacheWrite1hMult +
            Double(cacheRead) / 1e6 * inP * Stats.cacheReadMult

        apply(&total, input, output, w5, w1, cacheRead, cost)
        applyKey(&byModel, model, input, output, w5, w1, cacheRead, cost)
        applyKey(&byProject, project, input, output, w5, w1, cacheRead, cost)

        if let ts = root["timestamp"] as? String {
            lastActivity = ts
            if let date = parseTimestamp(ts) {
                let dayKey = dayFmt.string(from: date)
                applyKey(&byDay, dayKey, input, output, w5, w1, cacheRead, cost)
            }
        }
        return true
    }

    private func parseTimestamp(_ ts: String) -> Date? {
        if let d = isoParse.date(from: ts) { return d }
        // 小数秒なしの ISO8601 もフォールバックで受ける
        let alt = ISO8601DateFormatter()
        alt.formatOptions = [.withInternetDateTime]
        return alt.date(from: ts)
    }

    private func getInt(_ obj: [String: Any], _ name: String) -> Int {
        if let n = obj[name] as? NSNumber { return n.intValue }
        return 0
    }

    private func apply(_ b: inout Bucket, _ input: Int, _ output: Int, _ w5: Int, _ w1: Int, _ cacheRead: Int, _ cost: Double) {
        b.input += input
        b.output += output
        b.cacheWrite5m += w5
        b.cacheWrite1h += w1
        b.cacheRead += cacheRead
        b.cost += cost
        b.messages += 1
    }

    private func applyKey(_ map: inout [String: Bucket], _ key: String, _ input: Int, _ output: Int, _ w5: Int, _ w1: Int, _ cacheRead: Int, _ cost: Double) {
        var b = map[key] ?? Bucket()
        apply(&b, input, output, w5, w1, cacheRead, cost)
        map[key] = b
    }

    // index.html が期待する形の JSON 文字列を返す
    func snapshotJson() -> String {
        lock.lock()
        defer { lock.unlock() }

        let models = byModel
            .map { dto($0.value, name: $0.key) }
            .sorted { ($0["tokens"] as! Int) > ($1["tokens"] as! Int) }
        let projects = mergedProjects()

        let snap: [String: Any] = [
            "total": dto(total, name: nil),
            "models": models,
            "projects": projects,
            "daily": dailySeries(371),
            "lastActivity": lastActivity as Any,
            "startedAt": startedAt,
            "projectsDir": projectsDir,
            "intervalMs": intervalMs,
            "now": isoOut.string(from: Date()),
        ]
        guard let data = try? JSONSerialization.data(withJSONObject: snap, options: []),
              let s = String(data: data, encoding: .utf8) else { return "{}" }
        return s
    }

    private func addBucket(_ acc: inout Bucket, _ b: Bucket) {
        acc.input += b.input
        acc.output += b.output
        acc.cacheWrite5m += b.cacheWrite5m
        acc.cacheWrite1h += b.cacheWrite1h
        acc.cacheRead += b.cacheRead
        acc.cost += b.cost
        acc.messages += b.messages
    }

    // basename(真のルート) でグルーピングし、移動(存在1+消滅n)だけを1行へ統合する。呼び出しは lock 内。
    private func mergedProjects() -> [[String: Any]] {
        let fm = FileManager.default
        func canonical(_ folder: String) -> String { byProjectCanonical[folder] ?? folder }
        var existsCache: [String: Bool] = [:]
        func dirExists(_ path: String) -> Bool {
            if let c = existsCache[path] { return c }
            var isDir: ObjCBool = false
            let r = fm.fileExists(atPath: path, isDirectory: &isDir) && isDir.boolValue
            existsCache[path] = r
            return r
        }

        var groups: [String: [String]] = [:]
        for folder in byProject.keys {
            groups[Stats.baseName(canonical(folder)), default: []].append(folder)
        }

        var rows: [(name: String, bucket: Bucket)] = []
        for (_, folders) in groups {
            let existing = folders.filter { dirExists(canonical($0)) }
            let gone = folders.filter { !dirExists(canonical($0)) }
            if existing.count == 1 && gone.count >= 1 {
                let target = existing[0]
                var merged = Bucket()
                for f in folders { if let b = byProject[f] { addBucket(&merged, b) } }
                rows.append((displayPath(canonical(target)), merged))
            } else {
                for f in folders {
                    if let b = byProject[f] { rows.append((displayPath(canonical(f)), b)) }
                }
            }
        }
        return rows
            .sorted { $0.bucket.tokens > $1.bucket.tokens }
            .map { dto($0.bucket, name: $0.name) }
    }

    private func dto(_ b: Bucket, name: String?) -> [String: Any] {
        var d: [String: Any] = [
            "input": b.input,
            "output": b.output,
            "cacheWrite5m": b.cacheWrite5m,
            "cacheWrite1h": b.cacheWrite1h,
            "cacheRead": b.cacheRead,
            "cost": b.cost,
            "messages": b.messages,
            "tokens": b.tokens,
        ]
        d["name"] = name as Any? ?? NSNull()
        return d
    }

    // 直近 days 日分の日別系列（データの無い日も0埋め、古い→新しい順）。呼び出しは lock 内で。
    private func dailySeries(_ days: Int) -> [[String: Any]] {
        var list: [[String: Any]] = []
        list.reserveCapacity(days)
        let cal = Calendar.current
        let today = cal.startOfDay(for: Date())
        for i in stride(from: days - 1, through: 0, by: -1) {
            guard let day = cal.date(byAdding: .day, value: -i, to: today) else { continue }
            let key = dayFmt.string(from: day)
            if let b = byDay[key] {
                list.append(["date": key, "tokens": b.tokens, "cost": b.cost, "messages": b.messages])
            } else {
                list.append(["date": key, "tokens": 0, "cost": 0.0, "messages": 0])
            }
        }
        return list
    }
}
