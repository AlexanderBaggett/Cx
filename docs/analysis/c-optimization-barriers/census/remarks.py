import sys, re, glob, collections, json
KEEP = {'ClobberedBy', 'Type', 'Reason'}
def parse(path):
    with open(path, errors='replace') as f:
        doc = None
        for line in f:
            if line.startswith('--- !'):
                if doc: yield doc
                doc = {'kind': line[5:].strip(), 'args': []}
            elif doc is None: continue
            elif line.startswith('Pass:'): doc['pass'] = line.split(':',1)[1].strip()
            elif line.startswith('Name:'): doc['name'] = line.split(':',1)[1].strip()
            elif line.startswith('Function:'): doc['func'] = line.split(':',1)[1].strip()
            elif line.startswith('  - '):
                k, _, v = line[4:].partition(':')
                v = v.strip()
                if len(v) >= 2 and v[0] == "'" and v[-1] == "'": v = v[1:-1].replace("''", "'")
                doc['args'].append((k.strip(), v))
        if doc: yield doc
def msg(d):
    out = []
    for k, v in d['args']:
        if k == 'String': out.append(v)
        elif k in KEEP: out.append(v)
        else: out.append('<%s>' % k)
    return ''.join(out)
proj = sys.argv[1]; files = glob.glob(sys.argv[2])
bykind = collections.Counter(); byname = collections.Counter(); bymsg = collections.Counter()
funcs = set()
for p in files:
    for d in parse(p):
        key = (d['kind'], d.get('pass'), d.get('name'))
        bykind[d['kind']] += 1; byname[key] += 1
        funcs.add(d.get('func'))
        if d['kind'] in ('Missed', 'Analysis') and d.get('pass') in ('gvn', 'licm', 'loop-vectorize', 'inline', 'loop-idiom', 'slp-vectorizer', 'loop-unroll', 'sroa', 'memcpyopt', 'dse', 'loop-delete', 'indvars', 'argpromotion', 'globalopt'):
            m = msg(d)
            m = re.sub(r"'[^']*' not inlined into '[^']*'", "<callee> not inlined into <caller>", m)
            m = re.sub(r'\(cost=[^)]*\)', '(cost=N)', m)
            m = re.sub(r'at callsite .*', 'at callsite ...', m)
            bymsg[(d['kind'], d.get('pass'), d.get('name'), m)] += 1
json.dump({'proj': proj, 'bykind': bykind, 'byname': [[list(k), v] for k, v in byname.most_common()], 'bymsg': [[list(k), v] for k, v in bymsg.most_common()]}, open(sys.argv[3], 'w'))
print(proj, dict(bykind))
