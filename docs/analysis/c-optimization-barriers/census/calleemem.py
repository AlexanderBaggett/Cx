import re, sys, glob, collections
fn_attr = {}; groups = {}
calls = collections.defaultdict(set); 
call_re = re.compile(r'\b(?:call|invoke)\b[^@%]*?(@[\w.$]+|%[\w.$]+)\(')
for ll in glob.glob(sys.argv[1]):
    loc = {}; pend = []
    for ln in open(ll, errors='replace'):
        if ln.startswith('define '):
            m = re.search(r'@([\w.$]+)\(.*\)\s.*?#(\d+)', ln)
            if m: fn_attr[m.group(1)] = m.group(2)
        elif ln.startswith('attributes #'):
            m = re.match(r'attributes #(\d+) = \{(.*)\}', ln); groups[m.group(1)] = m.group(2)
        elif ln.startswith('!') and 'DILocation(' in ln:
            m = re.match(r'!(\d+) = !DILocation\(line: (\d+)(?:, column: (\d+))?', ln)
            if m: loc[m.group(1)] = (int(m.group(2)), int(m.group(3) or 0))
        elif 'call ' in ln or 'invoke ' in ln:
            m = call_re.search(ln); d = re.search(r'!dbg !(\d+)', ln)
            if m and d: pend.append((m.group(1), d.group(1)))
    for c, d in pend:
        if d in loc: calls[loc[d]].add(c)
def mem(fn):
    a = groups.get(fn_attr.get(fn, ''), '')
    m = re.search(r'memory\(([^)]*)\)', a)
    return m.group(1) if m else 'unknown (reads/writes any memory)'
def cls(s):
    if s.startswith('unknown'): return s
    if s == 'none': return 'none'
    parts = dict(p.split(': ') if ': ' in p else ('all', p) for p in s.split(', '))
    if 'all' in parts: 
        return 'any memory (' + parts['all'] + ')'
    return 'restricted: ' + ', '.join(sorted(parts))
res = collections.Counter()
for y in glob.glob(sys.argv[2]):
    for doc in open(y, errors='replace').read().split('\n--- !')[1:]:
        if not doc.startswith('Missed') or 'LoadClobbered' not in doc: continue
        m = re.search(r"ClobberedBy:\s+call\s*\n\s+DebugLoc:\s+\{ File: '[^']*', Line: (\d+), Column: (\d+) \}", doc)
        if not m: continue
        cs = [c for c in calls.get((int(m.group(1)), int(m.group(2))), ()) if c.startswith('@') and not c.startswith('@llvm.') and c[1:] in fn_attr]
        if len(cs) == 1: res[cls(mem(cs[0][1:]))] += 1
tot = sum(res.values())
for k, v in res.most_common(12): print(f'{v:8d} {100*v/tot:5.1f}%  {k}')
