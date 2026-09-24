#!/usr/bin/env python3
"""Run the head-to-head benchmarks.

For every round, the benchmarks are visited in a shuffled order. For each
benchmark, the toolchain binaries (c, cx, ctrl) run back to back in a shuffled
order, each in a fresh process pinned to one CPU. `ctrl` is a byte-identical
copy of the C binary, so c-vs-ctrl measures the noise floor.

Output: results/<timestamp>.jsonl. The first line is metadata; each further
line is one process run:
  {"round": r, "bench": name, "tc": "c"|"cx"|"ctrl", "ns": [...], "checksum": "0x..", "stable": true}
"""
import argparse, datetime, json, os, platform, random, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)


def sh(cmd):
    try:
        return subprocess.run(cmd, capture_output=True, text=True, check=True).stdout.strip()
    except Exception as e:  # noqa: BLE001 - metadata only
        return f"unavailable: {e}"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rounds", type=int, default=5)
    ap.add_argument("--iters", type=int, default=7)
    ap.add_argument("--cpu", default="3", help="CPU to pin every run to")
    ap.add_argument("--build", default=os.path.join(ROOT, "build"))
    ap.add_argument("--toolchains", default="c,cx,ctrl")
    ap.add_argument("--filter", default="", help="comma-separated benchmark names")
    ap.add_argument("--seed", type=int, default=None)
    a = ap.parse_args()

    tcs = a.toolchains.split(",")
    src = {tc: os.path.join(a.build, tc, "bench") for tc in tcs}
    for tc, b in src.items():
        if not os.access(b, os.X_OK):
            sys.exit(f"run.py: missing {b} (run `make` first)")
    # Run every toolchain's binary from a path of the same length (build/run/t0,
    # t1, ...). The program path is copied onto the initial stack, so paths of
    # different lengths shift stack alignment and can bias timings by a few
    # percent even for byte-identical binaries.
    rundir = os.path.join(a.build, "run")
    os.makedirs(rundir, exist_ok=True)
    bins = {}
    for i, tc in enumerate(tcs):
        dst = os.path.join(rundir, f"t{i}")
        shutil.copyfile(src[tc], dst)
        os.chmod(dst, 0o755)
        bins[tc] = dst

    lists = {tc: sh([b, "--list"]) for tc, b in bins.items()}
    if len(set(lists.values())) != 1:
        sys.exit("run.py: toolchains disagree on the benchmark list")
    names = [ln.split()[0] for ln in lists[tcs[0]].splitlines()]
    if a.filter:
        keep = set(a.filter.split(","))
        names = [n for n in names if n in keep]

    os.makedirs(os.path.join(ROOT, "results"), exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    path = os.path.join(ROOT, "results", f"{stamp}.jsonl")
    rnd = random.Random(a.seed)
    cats = {}
    for ln in lists[tcs[0]].splitlines():
        p = ln.split(None, 2)
        if len(p) >= 2:
            cats[p[0]] = p[1]
    buildinfo = {}
    for tc in tcs:
        bi = os.path.join(a.build, tc, "buildinfo.json")
        try:
            with open(bi) as f:
                buildinfo[tc] = json.load(f)
        except (OSError, ValueError):
            buildinfo[tc] = {"error": f"missing {bi}"}
    meta = {
        "meta": {
            "time": stamp, "rounds": a.rounds, "iters": a.iters, "cpu": a.cpu,
            "toolchains": tcs, "benchmarks": names, "categories": cats,
            "run_paths": {tc: os.path.relpath(bins[tc], ROOT) for tc in tcs},
            "machine": {"platform": platform.platform(), "processor": sh(["sh", "-c", "grep -m1 'model name' /proc/cpuinfo"]),
                        "nproc": os.cpu_count()},
            "buildinfo": buildinfo,
        }
    }
    with open(path, "w") as out:
        out.write(json.dumps(meta) + "\n")
        for r in range(a.rounds):
            order = names[:]
            rnd.shuffle(order)
            for i, name in enumerate(order):
                runs = tcs[:]
                rnd.shuffle(runs)
                for tc in runs:
                    p = subprocess.run(["taskset", "-c", a.cpu, bins[tc], name, "--iters", str(a.iters)],
                                       capture_output=True, text=True)
                    lines = [ln for ln in p.stdout.splitlines() if ln.startswith("{")]
                    if not lines:
                        rec = {"round": r, "bench": name, "tc": tc, "error": p.stderr.strip()[-500:], "rc": p.returncode}
                    else:
                        # an unstable checksum exits 1 but still prints the JSON line
                        j = json.loads(lines[-1])
                        rec = {"round": r, "bench": name, "tc": tc, "ns": j["ns"], "checksum": j["checksum"], "stable": j["stable"]}
                    out.write(json.dumps(rec) + "\n")
                    out.flush()
                print(f"\rround {r + 1}/{a.rounds}: {i + 1}/{len(order)} {name:<24}", end="", flush=True)
            print()
    print(f"results: {path}")


if __name__ == "__main__":
    main()
