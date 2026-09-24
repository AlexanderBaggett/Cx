"""Per-round composite speed ratio vs. same-round baseline.
Lua: geomean over 6 scripts; zstd: geomean over 4 MB/s metrics; SQLite: 1 metric.
Prints median, min, max over rounds and how many rounds were faster (>1.0)."""
import json, os, math, statistics as st, collections
os.chdir(os.path.join(os.environ['WORK'], 'abl'))
rows = [json.loads(l) for l in open('results.jsonl')]
D = collections.defaultdict(dict)
for r in rows: D[r['bin']][r['round']] = {k: v for k, v in r.items() if k not in ('bin', 'round')}
BASE = {'sqlite': 'sqlite-base', 'sqlite_sep': 'sqlite-base', 'lua': 'lua-base', 'lua_one': 'lua-base', 'zstd': 'zstd-base'}
sp = lambda k, v: 1 / v if k.endswith('_s') else v
geo = lambda xs: math.exp(sum(map(math.log, xs)) / len(xs))
print(f"{'variant':18s} {'median':>8s} {'min':>8s} {'max':>8s} faster")
for b in sorted(D):
    base = BASE[b.split('-')[0]]
    if b == base: continue
    per = [geo([sp(k, v) / sp(k, D[base][r][k]) for k, v in m.items()]) for r, m in D[b].items() if r in D[base]]
    print(f"{b:18s} {100*(st.median(per)-1):+7.1f}% {100*(min(per)-1):+7.1f}% {100*(max(per)-1):+7.1f}%  {sum(p > 1 for p in per)}/{len(per)}")
