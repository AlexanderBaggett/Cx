#!/usr/bin/env python3
"""Approximate checker for Cx rules 7.2.2 (mixed signedness after promotion,
non-constant operands) and 7.2.3 (implicit sign-changing or narrowing
conversions of non-constant values) over clang's JSON AST."""
import json, subprocess, sys, os
CLANG = os.environ.get("CLANG", os.path.expanduser("~/llvm-23.1.2-release/bin/clang"))
W = {'bool':1,'_Bool':1,'char':8,'signed char':8,'unsigned char':8,'short':16,'unsigned short':16,
     'int':32,'unsigned int':32,'long':64,'unsigned long':64,'long long':64,'unsigned long long':64}
def base(t):
    q = t.get('desugaredQualType', t.get('qualType'))
    q = q.replace('const ', '').strip()
    if q.startswith('enum'): return 'int'
    return q
def promo(q):
    if q in ('bool','_Bool','char','signed char','unsigned char','short','unsigned short'): return 'int'
    return q
def uns(q): return q.startswith('unsigned') or q in ('bool','_Bool')
ARITH = {'+','-','*','/','%','<','>','<=','>=','==','!=','&','|','^'}
def is_ice(n):
    k = n.get('kind')
    if k in ('IntegerLiteral','CharacterLiteral','UnaryExprOrTypeTraitExpr'): return True
    if k == 'DeclRefExpr': return n.get('referencedDecl',{}).get('kind') == 'EnumConstantDecl'
    if k in ('ConstantExpr','ParenExpr','ImplicitCastExpr','CStyleCastExpr','UnaryOperator','BinaryOperator','ConditionalOperator'):
        return all(is_ice(c) for c in n.get('inner', []))
    return False
def strip_ic(n):
    if n.get('kind') == 'ImplicitCastExpr' and n.get('castKind') == 'IntegralCast': return n['inner'][0]
    return n
cur = {'file': None, 'line': 0}
def upd(loc):
    if not isinstance(loc, dict): return
    for key in ('spellingLoc', 'expansionLoc'):
        if key in loc: upd(loc[key])
    if 'file' in loc: cur['file'] = loc['file']
    if 'line' in loc: cur['line'] = loc['line']
out = []
def rep(msg): 
    if cur['file'] and (cur['file'].startswith(('src/','harness/')) or '/bench/' in cur['file']): out.append(f"{os.path.relpath(cur['file'])}:{cur['line']}: {msg}")
def walk(n, parent=None):
    upd(n.get('loc')); r = n.get('range', {}); upd(r.get('begin'))
    k = n.get('kind')
    if k == 'BinaryOperator' and n.get('opcode') in ARITH and len(n.get('inner', [])) == 2:
        a, b = [strip_ic(c) for c in n['inner']]
        if 'type' in a and 'type' in b:
            ta, tb = promo(base(a['type'])), promo(base(b['type']))
            if ta in W and tb in W and uns(ta) != uns(tb) and not is_ice(a) and not is_ice(b):
                rep(f"7.2.2 mixed signedness '{n['opcode']}': {ta} vs {tb}")
    if k == 'CompoundAssignOperator' and n.get('opcode') not in ('<<=', '>>='):
        a, b = n['inner'][0], strip_ic(n['inner'][1])
        ta, tb = promo(base(a['type'])), promo(base(b['type']))
        if ta in W and tb in W and uns(ta) != uns(tb) and not is_ice(b):
            rep(f"7.2.2 mixed signedness '{n['opcode']}': {ta} vs {tb}")
    if k == 'ImplicitCastExpr' and n.get('castKind') == 'IntegralCast' and not (parent and parent.get('kind') == 'CompoundAssignOperator'):
        s = n['inner'][0]; ts, tt = base(s['type']), base(n['type'])
        if ts in W and tt in W and not is_ice(s):
            if not uns(ts) and uns(tt) and tt not in ('bool','_Bool'): rep(f"7.2.3 implicit signed->unsigned {ts} -> {tt}")
            elif W[tt] < W[ts]: rep(f"7.2.3 implicit narrowing {ts} -> {tt}")
            elif uns(ts) and not uns(tt) and W[tt] <= W[ts]: rep(f"7.2.3 implicit unsigned->signed {ts} -> {tt}")
    for c in n.get('inner', []): walk(c, n)
for f in sys.argv[1:]:
    cur['file'] = None
    j = json.loads(subprocess.run([CLANG, '-std=c23', '-Iharness', '-Isrc', '-Wno-unknown-attributes', '-x', 'c', '-Xclang', '-ast-dump=json', '-fsyntax-only', f], capture_output=True, text=True).stdout)
    walk(j)
for l in sorted(set(out)): print(l)
print(f"cx_scan: {'OK' if not out else str(len(set(out))) + ' finding(s)'} ({len(sys.argv) - 1} files)")
sys.exit(1 if out else 0)
