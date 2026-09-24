#!/usr/bin/env python3
"""compare_checksums.py A.jsonl B.jsonl: fail if any benchmark's checksum
differs between two `bench --all` outputs (e.g. the -O1 sanitizer build and
the -O2 build). A difference means a benchmark depends on unspecified or
optimization-dependent behaviour, which would also make C-vs-Cx comparisons
meaningless."""
import json, sys

def load(p):
    return {j["name"]: j["checksum"] for j in (json.loads(l) for l in open(p) if l.strip())}

a, b = load(sys.argv[1]), load(sys.argv[2])
bad = [n for n in a if n in b and a[n] != b[n]]
missing = sorted(set(a) ^ set(b))
for n in bad:
    print(f"checksum differs: {n}: {a[n]} vs {b[n]}")
for n in missing:
    print(f"only in one output: {n}")
print(f"compare_checksums: {len(a) - len(bad)}/{len(a)} identical")
sys.exit(1 if bad or missing else 0)
