#!/usr/bin/env python3
"""Summarize a results/<timestamp>.jsonl file as Markdown.

For each benchmark and round, a toolchain's time is the median of its timed
iterations. A round's speed ratio is t_C / t_X (> 1 means X is faster than C).
We report the median ratio over rounds, the range, and in how many rounds X
was faster. The `ctrl` toolchain is a byte-identical copy of the C binary, so
its ratios show the noise floor.

Verdict per benchmark:
  faster / slower   every round agrees and the median is outside the noise
                    band, where band = max(2%, largest |ctrl - 1| seen for
                    that benchmark)
  same              otherwise
"""
import glob, json, math, os, statistics as st, sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def load(path):
    meta, rows = None, []
    with open(path) as f:
        for ln in f:
            j = json.loads(ln)
            if "meta" in j:
                meta = j["meta"]
            else:
                rows.append(j)
    return meta, rows


def geo(xs):
    return math.exp(sum(math.log(x) for x in xs) / len(xs)) if xs else float("nan")


def pct(x):
    return f"{100 * (x - 1):+.1f}%"


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else max(glob.glob(os.path.join(ROOT, "results", "*.jsonl")), key=os.path.getmtime)
    meta, rows = load(path)
    cats = {}
    bench_bin = os.path.join(ROOT, "build", "c", "bench")
    if os.access(bench_bin, os.X_OK):
        import subprocess
        for ln in subprocess.run([bench_bin, "--list"], capture_output=True, text=True).stdout.splitlines():
            p = ln.split(None, 2)
            if len(p) >= 2:
                cats[p[0]] = p[1]

    t = defaultdict(lambda: defaultdict(dict))        # t[bench][tc][round] = median ns
    sums = defaultdict(set)                           # checksums per bench
    errors, unstable = [], []
    for r in rows:
        if "error" in r:
            errors.append(r)
            continue
        t[r["bench"]][r["tc"]][r["round"]] = st.median(r["ns"])
        sums[r["bench"]].add(r["checksum"])
        if not r["stable"]:
            unstable.append((r["bench"], r["tc"], r["round"]))

    tcs = [tc for tc in (meta or {}).get("toolchains", ["c", "cx", "ctrl"]) if tc != "c"]
    lines = []
    out = lines.append
    out(f"# Cx vs C benchmark results: `{os.path.basename(path)}`\n")
    if meta:
        out(f"- Rounds: {meta['rounds']}, timed iterations per process: {meta['iters']}, pinned to CPU {meta['cpu']}")
        out(f"- Machine: {meta['machine'].get('processor', '').split(':')[-1].strip()} ({meta['machine']['nproc']} CPUs), {meta['machine']['platform']}")
        out(f"- C compiler: {' '.join(meta['compilers']['c'])}")
        out(f"- Cx compiler: {' '.join(meta['compilers']['cx'])}; CX_FLAGS=`{meta.get('cx_flags', '')}`")
    out("")

    summary = {}
    for b in sorted(t):
        rounds = sorted(t[b]["c"])
        res = {"c_ms": st.median(t[b]["c"][r] for r in rounds) / 1e6}
        for tc in tcs:
            ratios = [t[b]["c"][r] / t[b][tc][r] for r in rounds if r in t[b][tc]]
            if not ratios:
                continue
            res[tc] = {"ms": st.median(t[b][tc][r] for r in rounds if r in t[b][tc]) / 1e6,
                       "med": st.median(ratios), "lo": min(ratios), "hi": max(ratios),
                       "faster": sum(x > 1 for x in ratios), "n": len(ratios)}
        band = 0.02
        if "ctrl" in res:
            band = max(band, abs(res["ctrl"]["lo"] - 1), abs(res["ctrl"]["hi"] - 1))
        res["band"] = band
        if "cx" in res:
            c = res["cx"]
            if c["faster"] == c["n"] and c["med"] > 1 + band:
                res["verdict"] = "**faster**"
            elif c["faster"] == 0 and c["med"] < 1 - band:
                res["verdict"] = "**slower**"
            else:
                res["verdict"] = "same"
        res["checksum_ok"] = len(sums[b]) == 1
        summary[b] = res

    # headline
    cx_meds = [s["cx"]["med"] for s in summary.values() if "cx" in s]
    ctrl_meds = [s["ctrl"]["med"] for s in summary.values() if "ctrl" in s]
    n_same = sum(s.get("verdict") == "same" for s in summary.values())
    out("## Headline\n")
    out(f"- Benchmarks: {len(summary)}; checksums identical across toolchains: {sum(s['checksum_ok'] for s in summary.values())}/{len(summary)}")
    out(f"- Geometric-mean speed, Cx vs C: **{pct(geo(cx_meds))}**; control (C vs identical copy): {pct(geo(ctrl_meds))}")
    out(f"- Verdicts: {n_same} same, {sum(s.get('verdict') == '**faster**' for s in summary.values())} faster, "
        f"{sum(s.get('verdict') == '**slower**' for s in summary.values())} slower")
    if errors:
        out(f"- **Errors: {len(errors)} runs failed** (first: {errors[0]['bench']}/{errors[0]['tc']}: {errors[0].get('error', '')[:120]})")
    if unstable:
        out(f"- **Unstable checksums** in {len(unstable)} runs: {unstable[:5]}")
    out("")

    by_cat = defaultdict(list)
    for b in summary:
        by_cat[cats.get(b, "?")].append(b)
    order = ["ops", "ds", "alg", "kern"] + sorted(set(by_cat) - {"ops", "ds", "alg", "kern"})
    for cat in order:
        if cat not in by_cat:
            continue
        cx_c = [summary[b]["cx"]["med"] for b in by_cat[cat] if "cx" in summary[b]]
        out(f"## {cat} ({len(by_cat[cat])} benchmarks; Cx vs C geomean {pct(geo(cx_c))})\n")
        out("| Benchmark | C ms | Cx ms | Cx vs C (median) | range | rounds faster | control (C copy) | verdict | checksum |")
        out("|---|---:|---:|---:|---:|---:|---:|---|---|")
        for b in sorted(by_cat[cat]):
            s = summary[b]
            cx = s.get("cx", {})
            ctrl = s.get("ctrl", {})
            out(f"| `{b}` | {s['c_ms']:.1f} | {cx.get('ms', float('nan')):.1f} | {pct(cx['med']) if cx else '—'} | "
                f"{pct(cx['lo']) if cx else ''} … {pct(cx['hi']) if cx else ''} | {cx.get('faster', '')}/{cx.get('n', '')} | "
                f"{pct(ctrl['med']) if ctrl else '—'} ({pct(ctrl['lo']) if ctrl else ''} … {pct(ctrl['hi']) if ctrl else ''}) | "
                f"{s.get('verdict', '—')} | {'ok' if s['checksum_ok'] else '**MISMATCH**'} |")
        out("")

    text = "\n".join(lines)
    print(text)
    with open(path[:-len(".jsonl")] + ".md", "w") as f:
        f.write(text + "\n")


if __name__ == "__main__":
    main()
