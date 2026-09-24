import subprocess, time, random, json, re, os, sys
os.chdir(os.path.join(os.environ['WORK'], 'abl'))  # holds bin/, luabench/, corpus.bin
R = int(sys.argv[1]) if len(sys.argv) > 1 else 5
bins = sorted(os.listdir('bin'))
proj = lambda b: b.split('-')[0]
out = open('results.jsonl', 'a')
PIN = ['taskset', '-c', '3']
def run_sqlite(b):
    t0 = time.perf_counter()
    subprocess.run(PIN + ['bin/' + b, '--memdb', '--size', '60', '--singlethread', '--nomemstat'], check=True, stdout=subprocess.DEVNULL)
    return {'sqlite_speedtest1_s': time.perf_counter() - t0}
def run_lua(b):
    r = {}
    for s in sorted(os.listdir('luabench')):
        t0 = time.perf_counter()
        subprocess.run(PIN + ['bin/' + b, 'luabench/' + s], check=True)
        r['lua_' + s[:-4] + '_s'] = time.perf_counter() - t0
    return r
def run_zstd(b):
    r = {}
    for lvl in (1, 3):
        o = subprocess.run(PIN + ['bin/' + b, '-b%d' % lvl, '-e%d' % lvl, '-i1', '-T1', 'corpus.bin'], check=True, capture_output=True, text=True)
        segs = [s for s in (o.stdout + o.stderr).replace('\n', '\r').split('\r') if s.count('MB/s') == 2]
        c, d = re.findall(r'([\d.]+) MB/s', segs[-1])
        r['zstd_L%d_comp_MBs' % lvl] = float(c); r['zstd_L%d_decomp_MBs' % lvl] = float(d)
    return r
RUN = {'sqlite': run_sqlite, 'sqlite_sep': run_sqlite, 'lua': run_lua, 'lua_one': run_lua, 'zstd': run_zstd}
for rnd in range(R):
    order = bins[:]; random.shuffle(order)
    for b in order:
        res = RUN[proj(b)](b)
        out.write(json.dumps({'round': rnd, 'bin': b, **res}) + '\n'); out.flush()
    print('round', rnd, 'done', time.strftime('%H:%M:%S'), flush=True)
