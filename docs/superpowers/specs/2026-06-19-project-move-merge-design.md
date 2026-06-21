# プロジェクト移動による重複の統合 設計書

- 日付: 2026-06-19
- 対象: ClaudeTokenMonitor（mac=Swift / linux=C / windows=C#）
- ステータス: 設計合意済み（実装前）

## 1. 背景・問題

各版は `~/.claude/projects/` 直下のフォルダ名を「プロジェクト名」キーにしてトークン使用量を集計し、
`web/index.html` がそのフォルダ名をそのまま表に描画する。

Claude Code はプロジェクトフォルダ名を「起動時の作業ディレクトリ（cwd）の絶対パスを
**英数字以外の文字をすべて `-` に置換**した文字列」で作る（`/` `.` `_` 等すべて `-`）。
そのためプロジェクトのディレクトリを移動すると、Claude Code 側に**新パス由来の別フォルダ**が作られ、
本ツールでは同一プロジェクトが**2 行に分かれて並ぶ**。

実例（ユーザー環境）:

| フォルダ | 真のルート | ディスク上 | basename |
|---|---|---|---|
| `-Users-kn-Desktop-AIRouter`（旧） | `/Users/kn/Desktop/AIRouter` | 消滅 | AIRouter |
| `-Users-kn-File-projects-AIRouter`（新） | `/Users/kn/File/projects/AIRouter` | 存在 | AIRouter |

これを 1 行に統合したい。

## 2. ゴール / 非ゴール

### ゴール
- 「ディレクトリ移動で生じた同一プロジェクトの重複」を自動で 1 行に統合する。
- 統合行の表示名を読みやすい実パス（`~` 短縮）にする。
- 3 版すべてで**同一ロジック**にする（挙動差を作らない）。

### 非ゴール
- `~/.claude/projects/` 配下の jsonl 等、Claude Code 本体のデータには一切手を入れない（物理統合はしない）。
- basename が同じだが別物のプロジェクト（例: `Webs/pulldownqr` と `Webs/ChromeExtentions/pulldownqr`、
  `autonomous/plan/chrome-1` と `autonomous/prototype/chrome-1`）は統合しない。
- 集計値（total / models / daily=日別・草）の意味は変えない。これらはプロジェクト横断の全体集計なので影響なし。

## 3. 確定した事実（実データ検証済み）

- フォルダ名のエンコード規則 = `[^A-Za-z0-9]` を `-` に置換（`tmp.60WZIn787j` → `tmp-60WZIn787j` で確認）。
  逆変換は不可能（ロッシー）。
- 集計対象である `assistant` 行には必ず `cwd`（実パス）が入っている。
  → `encode(cwd) == フォルダ名` となる cwd が、そのフォルダの**起動ディレクトリ＝真のルート**として一意に定まる。
- cwd を 1 つも持たないフォルダ（要約のみ等）はトークン 0 で集計に出ないため、統合対象に現れない。
- dedup（`seenIds`、メッセージ ID 単位）は全フォルダ横断で効いているため、フォルダ統合で**二重計上は起きない**。

## 4. アルゴリズム

統合は**スナップショット生成時**に行う。フォルダ単位の集計（`byProject`）は従来どおり保持し壊さない。

### 4.1 共通ヘルパ
- `encode(s)`: 文字ごとに、ASCII 英数字（`a-z A-Z 0-9`）はそのまま、それ以外（`/` `.` `_` 非ASCII 等すべて）を `-` に
  置換した文字列を返す。Claude Code 本体の `cwd.replace(/[^a-zA-Z0-9]/g, '-')` と同一規則（実データで `/` `.` を確認済み、
  `_`/非ASCII は同実装より同じ扱いと確定）。万一規則がズレても結果は「encode 不一致→統合しない」＝誤統合せず取りこぼすだけの安全側。
- `basename(path)`: 末尾の `/` を除き、最後の `/` 以降を返す（`/` が無ければ全体）。
- `displayPath(path)`: `home` を**実ユーザ home（`projectsDir` の上書き有無と無関係に
  `homeDirectoryForCurrentUser` / `g_get_home_dir()` / `SpecialFolder.UserProfile` で取得）**とし、
  `path == home` または `path` が `home + "/"` で始まる場合に home 接頭辞を `~` へ置換。それ以外はそのまま。
- `dirExists(path)`: `path` が実在するディレクトリなら真。同一スナップショット内では
  `(folder→canonical→存在)` を 1 回だけ評価して使い回す（毎フォルダの重複 stat を避ける）。
- 文字列比較は**バイト/序数比較**で統一する（C=`strcmp`、Swift=`<`(ASCII パス前提で序数一致)、C#=`string.CompareOrdinal`）。
  カルチャ依存比較は使わない。

### 4.2 真のルートパス復元（取り込み時、フォルダ単位・順序非依存）
行を処理する際、その行の `cwd` と所属フォルダ `folder` から、フォルダごとに
`canonical`（代表 cwd 文字列）と `canonicalMatches`（`encode(canonical)==folder` か）を O(1) 状態で更新する。
**`cwd` の捕捉は root を parse した直後・全 early return（type 不一致 / usage 無 / model 不正 / `seenIds` 重複 等）より前**に行い、
集計可否や dedup と独立させる（識別をトークン有無に依存させない）。`cwd` を持つ行ならフォルダ識別に使う:

```
matches = (encode(cwd) == folder)
if canonical 未設定:
    canonical = cwd; canonicalMatches = matches
else if matches && !canonicalMatches:
    canonical = cwd; canonicalMatches = true          # 一致は非一致に勝つ
else if matches == canonicalMatches && cwd < canonical:
    canonical = cwd                                    # 同条件なら辞書順最小（走査順非依存）
```

結果: 「encode 一致する cwd を優先、その中で辞書順最小。一致が無ければ全 cwd の辞書順最小」。
ファイル走査順に依存しない決定的な値になる。`cwd` が皆無のフォルダはフォールバックで `canonical = folder` とする。

### 4.3 グルーピングと ghost-fold（スナップショット時）
```
# 入力: byProject(folder→Bucket), canonical(folder→path)
groups = {}                       # key = basename(canonical[folder])
for folder in byProject: groups[basename(canonical[folder])].append(folder)

rows = []                         # (displayName, mergedBucket)
for key, folders in groups:
    existing = [f for f in folders if dirExists(canonical[f])]
    gone     = [f for f in folders if not dirExists(canonical[f])]
    if len(existing) == 1 and len(gone) >= 1:
        target = existing[0]
        merged = Σ Bucket(f) for f in folders          # 移動: 全部を 1 行へ
        rows.append( (displayPath(canonical[target]), merged) )
    else:
        for f in folders:                              # それ以外は各自独立
            rows.append( (displayPath(canonical[f]), Bucket(f)) )
rows.sort(by tokens desc)
```

判定の意味:
- **存在 1 + 消滅 ≥1** → 移動とみなし統合（代表＝存在側の実パス）。
- **存在 0**（全部消滅・別物の削除済み等）→ 統合しない。
- **存在 ≥2**（同名で両方実在）→ どれが移動先か不明なので統合しない。

### 4.4 出力
- `projects` 配列の各要素 `name` を `displayName`（`~` 短縮実パス）にし、`tokens/messages/cost` 等は合算値、tokens 降順で出力。
- `total` / `models` / `daily` は変更なし。

## 5. 各版の実装方針（同一ロジック）

| | mac (Swift / `mac/Sources/Stats.swift`) | linux (C / `linux/src/stats.c`) | windows (C# / `windows/desktop/Stats.cs`) |
|---|---|---|---|
| cwd 取得 | `processLine` で `root["cwd"]` を読む | `process_line` で `root` の `cwd` を読む | `ProcessLine` で `root.cwd` を読む |
| 代表 cwd 状態 | `byProjectCanonical:[String:String]` + `byProjectCanonicalMatch:[String:Bool]` | `by_project_canonical`(GHashTable folder→char*) + `by_project_canonical_match`(folder→gboolean) | `_byProjectCanonical:Dictionary<string,string>` + `_byProjectCanonicalMatch:Dictionary<string,bool>` |
| encode/basename/displayPath/dirExists | Swift で実装 | C で実装（`g_file_test(..., IS_DIR)`） | C# で実装（`Directory.Exists`） |
| グルーピング | `snapshotJson()` の projects 構築を差し替え | `add_sorted_map` 相当を projects 専用の統合版に差し替え | `SnapshotJson()` の projects 構築を差し替え |

- `processLine` 系のシグネチャに cwd を渡す必要があるため、`ingestFile`→`processLine` の引数に cwd を追加するか、
  `processLine` 内で root から直接読む（後者が変更小）。**root から直接読む**方針。
- home は各版が `projectsDir` 既定値を作るのに使っているユーザ home と同じものを用いる。

## 6. フロントエンド

`web/index.html` は `p.name` をそのまま描画するため**変更不要**。実パスがそのままラベル／表に出る。
（3 版の index.html は同一内容。`mac/dist/` 配下はビルド成果物のため対象外。）

## 7. 既知の限界（許容済み）

basename が同じ別プロジェクトのうち**片方だけがディスク上から消えている**場合、
「存在 1 + 消滅 1」と判定され、消えた側が残った側へ誤統合され得る（移動と区別不能なため）。
発生条件は限定的であり、ユーザ承知のうえで許容する。

## 8. 検証計画

- 参照実装（Python）で実データから「期待される統合後 projects 一覧」を算出しておく。
- 期待される結果（実データ）:
  - `AIRouter`: Desktop（消滅）+ File/projects（存在）→ 1 行に統合。表示 `~/File/projects/AIRouter`。
  - `pulldownqr`（両方実在）: 統合されず別行のまま、各実パス表示。
  - `autonomous/plan/* と prototype/*`（両方消滅）: 統合されない。
- mac 版は darwin 上でビルド・起動し、`/snapshot` 相当の JSON（または UI のプロジェクト表）が参照実装と一致することを確認。
- linux/windows 版はこの環境で実行できないため、ロジック等価性をレビューで担保（3 版の差分が encode/basename/displayPath/grouping で同一であること）。

## 9. スコープ外

- `~/.claude/projects/` のデータ物理統合（AIRouter 含む。アルゴリズムが自動統合するため不要）。
- 統合元フォルダ数のツールチップ表示等の付加 UI（YAGNI。必要なら別途）。
