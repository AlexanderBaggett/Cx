#!/usr/bin/env python3
"""Summarize a results/<timestamp>.jsonl file as Markdown.

For each benchmark and round, a toolchain's time is the median of its timed
iterations. A round's speed ratio is t_C / t_X (> 1 means X is faster than C).
We report the median ratio over rounds, its range, and in how many rounds X
was faster.

The `ctrl` toolchain is a byte-identical copy of the C binary. It gets the
same verdict rule as Cx, so its verdicts count the *false positives* the rule
produces on this machine. Every Cx verdict should be read against that count.

Verdict rule (needs at least MIN_ROUNDS rounds):
  faster / slower   every round agrees and the median lies outside the noise
                    band, where band = max(2%, the largest |ctrl - 1| seen for
                    that benchmark)
  same              otherwise

Benchmarks whose time is dominated by the C library (identical for both
toolchains) are tagged "libc" and left out of the compiler-bound geomean.
"""
import glob, json, math, os, statistics as st, sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
MIN_ROUNDS = 3
LIBC_BOUND = {"str_libc", "mem_copy", "mem_set_cmp", "alloc_churn", "sort_qsort",
              "fp_libm", "fmt_snprintf", "slist"}


def load(path):
    meta, rows = None, []
    with open(path) as f:
        for ln in f:
            if not ln.strip():
                continue
            j = json.loads(ln)
            if "meta" in j:
                meta = j["meta"]
            else:
                rows.append(j)
    return meta or {}, rows


def geo(xs):
    xs = [x for x in xs if x and x > 0]
    return math.exp(sum(math.log(x) for x in xs) / len(xs)) if xs else float("nan")


def pct(x):
    return "—" if x is None or x != x else f"{100 * (x - 1):+.1f}%"


def verdict(res, band):
    if res is None or res["n"] < MIN_ROUNDS:
        return "n/a"
    if res["faster"] == res["n"] and res["med"] > 1 + band:
        return "faster"
    if res["faster"] == 0 and res["med"] < 1 - band:
        return "slower"
    return "same"


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else max(glob.glob(os.path.join(ROOT, "results", "*.jsonl")), key=os.path.getmtime)
    meta, rows = load(path)
    cats = meta.get("categories", {})

    t = defaultdict(lambda: defaultdict(dict))        # t[bench][tc][round] = median ns
    sums = defaultdict(set)                           # checksums per bench
    errors, unstable = [], []
    for r in rows:
        if "error" in r or not r.get("ns"):
            errors.append(r)
            continue
        t[r["bench"]][r["tc"]][r["round"]] = st.median(r["ns"])
        sums[r["bench"]].add(r["checksum"])
        if not r.get("stable", True):
            unstable.append((r["bench"], r["tc"], r["round"]))

    tcs = [tc for tc in meta.get("toolchains", ["c", "cx", "ctrl"]) if tc != "c"]
    summary = {}
    for b in sorted(t):
        if not t[b].get("c"):
            continue
        rounds = sorted(t[b]["c"])
        res = {"c_ms": st.median(t[b]["c"][r] for r in rounds) / 1e6}
        for tc in tcs:
            rr = [r for r in rounds if r in t[b].get(tc, {})]
            if not rr:
                continue
            ratios = [t[b]["c"][r] / t[b][tc][r] for r in rr]
            res[tc] = {"ms": st.median(t[b][tc][r] for r in rr) / 1e6,
                       "med": st.median(ratios), "lo": min(ratios), "hi": max(ratios),
                       "faster": sum(x > 1 for x in ratios), "n": len(ratios)}
        band = 0.02
        if "ctrl" in res:
            band = max(band, abs(res["ctrl"]["lo"] - 1), abs(res["ctrl"]["hi"] - 1))
        res["band"] = band
        res["verdict"] = verdict(res.get("cx"), band)
        # the control is judged with the plain 2% band (it cannot use itself)
        res["ctrl_verdict"] = verdict(res.get("ctrl"), 0.02)
        res["checksum_ok"] = len(sums[b]) == 1
        summary[b] = res

    lines = []
    out = lines.append
    out(f"# Cx vs C benchmark results: `{os.path.basename(path)}`\n")
    bi = meta.get("buildinfo", {})
    out(f"- Rounds: {meta.get('rounds')}, timed iterations per process: {meta.get('iters')}, pinned to CPU {meta.get('cpu')}")
    m = meta.get("machine", {})
    out(f"- Machine: {m.get('processor', '').split(':')[-1].strip()} ({m.get('nproc')} CPUs), {m.get('platform')}")
    for tc in ("c", "cx"):
        if tc in bi:
            out(f"- **{tc.upper()}** compiler: {bi[tc].get('version', '?')}  ")
            out(f"  flags: `{bi[tc].get('flags', '?')}`" + (f"; Cx overrides: `{bi[tc]['overrides']}`" if bi[tc].get('overrides') else ""))
    out("")

    allb = list(summary.values())
    comp = [s for b, s in summary.items() if b not in LIBC_BOUND]
    def count(key, v):
        return sum(s.get(key) == v for s in allb)
    out("## Headline\n")
    out(f"- Benchmarks: {len(allb)} ({len(comp)} compiler-bound, {len(allb) - len(comp)} C-library-bound)")
    out(f"- Checksums identical across toolchains in every run: **{sum(s['checksum_ok'] for s in allb)}/{len(allb)}**")
    out(f"- Geometric-mean speed, Cx vs C: **{pct(geo([s['cx']['med'] for s in comp if 'cx' in s]))}** compiler-bound, "
        f"{pct(geo([s['cx']['med'] for s in allb if 'cx' in s]))} all")
    out(f"- Control (C vs a byte-identical copy of C): {pct(geo([s['ctrl']['med'] for s in comp if 'ctrl' in s]))} compiler-bound, "
        f"{pct(geo([s['ctrl']['med'] for s in allb if 'ctrl' in s]))} all")
    out(f"- Cx verdicts: {count('verdict', 'same')} same, {count('verdict', 'faster')} faster, {count('verdict', 'slower')} slower"
        + (f", {count('verdict', 'n/a')} n/a (fewer than {MIN_ROUNDS} rounds)" if count('verdict', 'n/a') else ""))
    out(f"- Control false positives with the same rule: {count('ctrl_verdict', 'faster') + count('ctrl_verdict', 'slower')} of {len(allb)}")
    if errors:
        e = errors[0]
        out(f"- **{len(errors)} runs failed** (first: {e.get('bench')}/{e.get('tc')}: {str(e.get('error', ''))[:160]})")
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
        out("| Benchmark | C ms | Cx ms | Cx vs C (median) | range over rounds | rounds Cx faster | control (median, range) | verdict | checksums |")
        out("|---|---:|---:|---:|---:|---:|---:|---|---|")
        for b in sorted(by_cat[cat]):
            s = summary[b]
            cx, ctrl = s.get("cx"), s.get("ctrl")
            tag = " (libc)" if b in LIBC_BOUND else ""
            v = s["verdict"] if s["verdict"] in ("same", "n/a") else f"**{s['verdict']}**"
            out(f"| `{b}`{tag} | {s['c_ms']:.1f} | {cx['ms'] if cx else float('nan'):.1f} | {pct(cx['med']) if cx else '—'} | "
                f"{pct(cx['lo']) if cx else ''} … {pct(cx['hi']) if cx else ''} | {cx['faster'] if cx else ''}/{cx['n'] if cx else ''} | "
                f"{pct(ctrl['med']) if ctrl else '—'} ({pct(ctrl['lo']) if ctrl else ''} … {pct(ctrl['hi']) if ctrl else ''}) | "
                f"{v} | {'ok' if s['checksum_ok'] else '**MISMATCH**'} |")
        out("")

    text = "\n".join(lines)
    print(text)
    with open(path[:-len(".jsonl")] + ".md", "w") as f:
        f.write(text + "\n")


if __name__ == "__main__":
    main()
