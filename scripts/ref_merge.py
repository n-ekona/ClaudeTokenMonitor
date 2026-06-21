#!/usr/bin/env python3
"""参照実装: 仕様 4.1-4.3 のアルゴリズムで「統合後 projects」を算出する。
ネイティブ各版(Swift/C/C#)の出力検証用。集計は assistant+usage 行のみ、
seenIds(メッセージID)で全フォルダ横断 dedup。"""
import json, glob, os, re, sys

BASE = os.environ.get("CLAUDE_PROJECTS_DIR") or os.path.expanduser("~/.claude/projects")
HOME = os.path.expanduser("~")

PRICING = [
    ("claude-fable-5", 10, 50), ("claude-mythos-5", 10, 50),
    ("claude-opus-4-8", 5, 25), ("claude-opus-4-7", 5, 25),
    ("claude-opus-4-6", 5, 25), ("claude-opus-4-5", 5, 25),
    ("claude-opus-4-1", 15, 75), ("claude-opus-4-0", 15, 75),
    ("claude-sonnet-4-6", 3, 15), ("claude-sonnet-4-5", 3, 15),
    ("claude-sonnet-4-0", 3, 15), ("claude-haiku-4-5", 1, 5),
    ("claude-3-5-haiku", 0.8, 4), ("claude-3-haiku", 0.25, 1.25),
]
def price_for(m):
    for k, i, o in PRICING:
        if m == k or m.startswith(k):
            return i, o
    return 5, 25

def encode(s):  # Claude Code: [^A-Za-z0-9] -> '-'
    return re.sub(r'[^A-Za-z0-9]', '-', s)

def basename(p):
    p = p.rstrip('/')
    i = p.rfind('/')
    return p[i+1:] if i >= 0 else p

def display_path(p):
    if p == HOME:
        return "~"
    if p.startswith(HOME + "/"):
        return "~" + p[len(HOME):]
    return p

def get_int(d, k):
    v = d.get(k)
    return v if isinstance(v, int) else 0

def main():
    seen = set()
    by_project = {}          # folder -> bucket(dict)
    canonical = {}           # folder -> cwd
    canon_match = {}         # folder -> bool

    def bucket():
        return dict(input=0, output=0, cw5=0, cw1=0, cread=0, messages=0, cost=0.0)

    for d in sorted(glob.glob(os.path.join(BASE, "*"))):
        if not os.path.isdir(d):
            continue
        folder = os.path.basename(d)
        for fp in sorted(glob.glob(os.path.join(d, "**", "*.jsonl"), recursive=True)):
            try:
                f = open(fp, encoding="utf-8", errors="replace")
            except OSError:
                continue
            with f:
                for line in f:
                    line = line.strip()
                    if not line:
                        continue
                    try:
                        o = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    if not isinstance(o, dict):
                        continue
                    # --- canonical 捕捉(全 early return より前, type 不問) ---
                    c = o.get("cwd")
                    if isinstance(c, str) and c:
                        m = (encode(c) == folder)
                        if folder not in canonical:
                            canonical[folder] = c; canon_match[folder] = m
                        else:
                            cm = canon_match[folder]
                            if m and not cm:
                                canonical[folder] = c; canon_match[folder] = True
                            elif m == cm and c < canonical[folder]:
                                canonical[folder] = c
                    # --- 集計 ---
                    if o.get("type") != "assistant":
                        continue
                    msg = o.get("message")
                    if not isinstance(msg, dict):
                        continue
                    usage = msg.get("usage")
                    if not isinstance(usage, dict):
                        continue
                    model = msg.get("model")
                    if not model or model == "<synthetic>":
                        continue
                    mid = msg.get("id")
                    if isinstance(mid, str) and mid:
                        if mid in seen:
                            continue
                        seen.add(mid)
                    inp = get_int(usage, "input_tokens")
                    out = get_int(usage, "output_tokens")
                    cread = get_int(usage, "cache_read_input_tokens")
                    ccreate = get_int(usage, "cache_creation_input_tokens")
                    w5 = w1 = 0
                    cc = usage.get("cache_creation")
                    if isinstance(cc, dict):
                        w5 = get_int(cc, "ephemeral_5m_input_tokens")
                        w1 = get_int(cc, "ephemeral_1h_input_tokens")
                    else:
                        w5 = ccreate
                    ip, op = price_for(model)
                    cost = inp/1e6*ip + out/1e6*op + w5/1e6*ip*1.25 + w1/1e6*ip*2.0 + cread/1e6*ip*0.1
                    b = by_project.setdefault(folder, bucket())
                    b["input"] += inp; b["output"] += out; b["cw5"] += w5
                    b["cw1"] += w1; b["cread"] += cread; b["cost"] += cost
                    b["messages"] += 1

    # canonical 未設定(=cwd皆無)のフォルダはフォールバック
    for folder in by_project:
        canonical.setdefault(folder, folder)

    def tokens(b):
        return b["input"] + b["output"] + b["cw5"] + b["cw1"] + b["cread"]

    # --- グルーピング & ghost-fold ---
    groups = {}
    for folder in by_project:
        groups.setdefault(basename(canonical[folder]), []).append(folder)

    rows = []
    for key, folders in groups.items():
        existing = [f for f in folders if os.path.isdir(canonical[f])]
        gone = [f for f in folders if not os.path.isdir(canonical[f])]
        if len(existing) == 1 and len(gone) >= 1:
            target = existing[0]
            merged = bucket()
            for f in folders:
                b = by_project[f]
                for k in merged:
                    merged[k] += b[k]
            rows.append((display_path(canonical[target]), merged, sorted(folders)))
        else:
            for f in folders:
                rows.append((display_path(canonical[f]), by_project[f], [f]))

    rows.sort(key=lambda r: tokens(r[1]), reverse=True)

    out = [{"name": n, "tokens": tokens(b), "messages": b["messages"],
            "cost": round(b["cost"], 6), "folders": fs} for n, b, fs in rows]
    print(json.dumps(out, ensure_ascii=False, indent=2))

if __name__ == "__main__":
    main()
