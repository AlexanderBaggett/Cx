import re, sys, glob, collections
# usage: callkinds.py "<ll glob>" "<yaml glob>"
defined, declared = set(), set()
calls = collections.defaultdict(set)   # (file-basename, line, col) -> kinds
dil = {}
call_re = re.compile(r'\b(?:tail |musttail |notail )?(?:call|invoke)\b[^@%]*?(@[\w.$]+|%[\w.$]+)\(')
for ll in glob.glob(sys.argv[1]):
    base = None; lines = open(ll, errors='replace').read().split('\n')
    loc = {}; pend = []
    for ln in lines:
        if ln.startswith('define '):
            m = re.search(r'@([\w.$]+)\(', ln); defined.add(m.group(1))
        elif ln.startswith('declare '):
            m = re.search(r'@([\w.$]+)\(', ln); declared.add(m.group(1))
        elif ln.startswith('!') and 'DILocation(' in ln:
            m = re.match(r'!(\d+) = !DILocation\(line: (\d+)(?:, column: (\d+))?', ln)
            if m: loc[m.group(1)] = (int(m.group(2)), int(m.group(3) or 0))
        elif ' call ' in ln or ' invoke ' in ln or ln.lstrip().startswith(('call ', 'tail call ')):
            m = call_re.search(ln); d = re.search(r'!dbg !(\d+)', ln)
            if m and d: pend.append((m.group(1), d.group(1)))
    for callee, dbg in pend:
        if dbg in loc: calls[loc[dbg]].add(callee)
def kind(callee):
    if callee.startswith('%'): return 'indirect (function pointer)'
    n = callee[1:]
    if n.startswith('llvm.'): return 'intrinsic'
    if n in defined: return 'defined in this TU, not inlined'
    return 'external declaration (other TU / libc)'
res = collections.Counter(); unmatched = 0
for y in glob.glob(sys.argv[2]):
    txt = open(y, errors='replace').read()
    for doc in txt.split('\n--- !')[1:]:
        if not doc.startswith('Missed') or 'Name:            LoadClobbered' not in doc: continue
        m = re.search(r"ClobberedBy:\s+call\s*\n\s+DebugLoc:\s+\{ File: '[^']*', Line: (\d+), Column: (\d+) \}", doc)
        if not m: continue
        ks = {kind(c) for c in calls.get((int(m.group(1)), int(m.group(2))), set())}
        ks.discard('intrinsic')
        if not ks: unmatched += 1
        elif len(ks) == 1: res[ks.pop()] += 1
        else: res['mixed'] += 1
tot = sum(res.values())
for k, v in res.most_common(): print(f'{v:8d} {100*v/tot:5.1f}%  {k}')
print(f'(classified {tot}, unmatched {unmatched})')
