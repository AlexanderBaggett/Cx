import sys, glob, os, subprocess, collections, re
# run callkinds/calleemem per TU pair and aggregate the counts
script, lldir, ydir = sys.argv[1], sys.argv[2], sys.argv[3]
agg = collections.Counter()
for ll in sorted(glob.glob(lldir + '/*.ll')):
    y = os.path.join(ydir, os.path.basename(ll)[:-3] + '.yaml')
    if not os.path.exists(y): continue
    out = subprocess.run(['python3', script, ll, y], capture_output=True, text=True).stdout
    for m in re.finditer(r'^\s*(\d+)\s+[\d.]+%\s+(.+)$', out, re.M): agg[m.group(2)] += int(m.group(1))
tot = sum(agg.values())
for k, v in agg.most_common(): print(f'{v:8d} {100*v/tot:5.1f}%  {k}')
