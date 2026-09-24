"""Summarize results.jsonl: per variant, paired-by-round speed change vs the
project's baseline. Positive = faster. Times are converted to speed (1/t)."""
import json, os, sys, statistics as st, collections, math
os.chdir(os.path.join(os.environ['WORK'], 'abl'))
rows = [json.loads(l) for l in open('results.jsonl')]
data = collections.defaultdict(dict)          # bin -> round -> metrics
for r in rows: data[r['bin']][r['round']] = {k: v for k, v in r.items() if k not in ('bin', 'round')}
def speed(k, v): return 1.0 / v if k.endswith('_s') else v
BASE = {'sqlite': 'sqlite-base', 'sqlite_sep': 'sqlite-base', 'lua': 'lua-base', 'lua_one': 'lua-base', 'zstd': 'zstd-base'}
def summary(b):
    base = BASE[b.split('-')[0]]
    per_metric = collections.defaultdict(list)
    for rnd, m in data[b].items():
        if rnd not in data[base]: continue
        for k, v in m.items():
            per_metric[k].append(speed(k, v) / speed(k, data[base][rnd][k]))
    return {k: (st.median(v), min(v), max(v), len(v)) for k, v in per_metric.items()}
def geo(xs): return math.exp(sum(math.log(x) for x in xs) / len(xs))
print(f"{'binary':22s} {'metric':26s} {'median':>8s} {'min':>8s} {'max':>8s}  n")
for b in sorted(data):
    s = summary(b)
    for k in sorted(s):
        med, lo, hi, n = s[k]
        print(f"{b:22s} {k:26s} {100*(med-1):+7.1f}% {100*(lo-1):+7.1f}% {100*(hi-1):+7.1f}%  {n}")
    if b.startswith('lua'):
        print(f"{b:22s} {'lua_geomean':26s} {100*(geo([v[0] for v in s.values()])-1):+7.1f}%")
