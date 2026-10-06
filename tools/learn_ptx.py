"""Learn the ptx conversion tables from the game's legacy/Enhanced particle files (numpy, fast).
Outputs learned.json: tmap (legacy type id -> Enhanced id of the main set), tables (shader|technique -> Enhanced
technique index and variable list), defaults (shader|technique#hash -> payload hex from +0x20)."""
import struct, glob, collections, json, re
import numpy as np
from ptx_common import load, ptr

def qwords(x):
    n = len(x) // 8
    return np.frombuffer(x[:n * 8], dtype='<u8')

TYLO, TYHI = 0x140000000, 0x150000000
# 1. type map votes (from pwalk4 output)
votes = {}
for l in open('pwalk4.txt')  # output of ptx_pairwalk.py:
    if l.startswith('diffs'):
        break
    m = re.match(r"\s+0x([0-9a-f]+) \[(.*)\]$", l)
    if m:
        votes[int(m.group(1), 16)] = [(int(a, 16), int(n)) for a, n in re.findall(r"\('0x([0-9a-f]+)', (\d+)\)", m.group(2))]
# Main Enhanced set: ids in files whose root object is 0x1406e2a40
Q = set()
efiles = glob.glob('../yptall/E/*/*.ypt')
cache = {}
for f in efiles:
    x, vs = load(f); cache[f] = (x, vs)
    if struct.unpack_from('<Q', x, 0)[0] != 0x1406e2a40:
        continue
    w = qwords(x)
    Q |= set(int(v) for v in np.unique(w[(w >= TYLO) & (w < TYHI)]))
tmap = {}
for lt, cands in votes.items():
    q = [e for e, n in cands if e in Q]
    if q:
        tmap[lt] = q[0]
    elif cands and cands[0][0] != lt:
        tmap[lt] = cands[0][0]  # no id of the main set: the id the game's files use most
print('type map', len(tmap), 'of', len(votes), 'unmapped', [hex(t) for t in votes if t not in tmap])

LEG_RULES = (0x14060cac8, 0x14060aaa8)
def s(x, vs, v):
    p = ptr(vs, v)
    return x[p:p + 64].split(b'\0')[0].decode('latin1') if p is not None and p < len(x) else ''
def rule_offsets(x, vs, legacy):
    w = qwords(x)
    if legacy:
        idx = np.nonzero(np.isin(w, np.array(LEG_RULES, dtype='<u8')))[0]
    else:
        # Enhanced rules: typed object with typed embedded objects at +0x1b0 and +0x1d8 and a "ptfx" shader name
        typed = (w >= TYLO) & (w < TYHI)
        cand = np.nonzero(typed[:-0x3c] & typed[0x36:len(typed) - 6] & typed[0x3b:len(typed) - 1])[0]
        idx = cand
    out = []
    for i in idx:
        o = int(i) * 8
        if o + 0x200 > len(x):
            continue
        sh = s(x, vs, struct.unpack_from('<Q', x, o + 0x1b8)[0])
        if sh.startswith('ptfx'):
            out.append(o)
    return out

tables = {}; defaults = collections.defaultdict(collections.Counter); leg_keys = collections.Counter()
for f in efiles:
    x, vs = cache[f]
    q = lambda o: struct.unpack_from('<Q', x, o)[0]
    for o in rule_offsets(x, vs, False):
        key = s(x, vs, q(o + 0x1b8)) + '|' + s(x, vs, q(o + 0x1c0))
        arr = ptr(vs, q(o + 0x1f0)); n = q(o + 0x1f8) & 0xffff
        if arr is None:
            continue
        vars_ = []
        for i in range(n):
            vo = ptr(vs, q(arr + 8 * i))
            h, ty, idx, fl = struct.unpack_from('<IIII', x, vo + 0x10)
            vars_.append((h, ty, idx, fl))
            defaults[key + '#' + hex(h)][x[vo + 0x20:vo + 0x40].hex()] += 1
        tables.setdefault(key, collections.Counter())[(q(o + 0x1d0) & 0xffffffff, tuple(vars_))] += 1
for f in glob.glob('../yptall/L/*/*.ypt'):
    x, vs = load(f)
    for o in rule_offsets(x, vs, True):
        q = lambda a: struct.unpack_from('<Q', x, a)[0]
        leg_keys[s(x, vs, q(o + 0x1b8)) + '|' + s(x, vs, q(o + 0x1c0))] += 1
print('enhanced techniques', len(tables), 'legacy techniques', len(leg_keys))
print('legacy techniques without an Enhanced table:', [k for k in leg_keys if k not in tables])
for k, c in tables.items():
    if len(c) > 1:
        print('inconsistent', k, [(v[0], len(v[1]), n) for v, n in c.most_common()])
json.dump({'tmap': {hex(a): hex(b) for a, b in tmap.items()},
           'tables': {k: [[v[0], [list(t) for t in v[1]]], n] for k, c in tables.items() for v, n in c.most_common(1)},
           'defaults': {k: c.most_common(1)[0][0] for k, c in defaults.items()}}, open('learned.json', 'w'))
