"""Typed parallel walk of legacy/Enhanced ypt pairs (../yptall/pairs.txt, L/ and E/ from debug extractall):
prints legacy -> Enhanced type id votes and remaining field differences. Output feeds learn_ptx.py (pwalk4.txt)."""
import struct, glob, os, collections, json
from ptx_common import load, ptr
S=json.load(open('sizes.json'))
ES={int(k,16):v[1] for k,v in S['E'].items()}; LS={int(k,16):v[1] for k,v in S['L'].items()}
TY=lambda v: 0x140000000<=v<0x150000000
diffs=collections.defaultdict(collections.Counter); tmap=collections.defaultdict(collections.Counter); cnt=collections.Counter()
SKIP=set()  # filled below
RULES=set()  # rule shader vars: rebuilt
def walk(L,lv,E,ev,lo,eo,seen,owner):
    if (lo,eo) in seen: return
    seen.add((lo,eo))
    q=lambda x,o: struct.unpack_from('<Q',x,o)[0] if o+8<=len(x) else 0
    lt,et=q(L,lo),q(E,eo)
    typed=TY(lt) and TY(et)
    if typed: tmap[lt][et]+=1; cnt[lt]+=1; W=max(8,(ES.get(et,0x10)//8)*8); owner=lt
    else: W=0x10
    for i in range(0,W,8):
        a,c=q(L,lo+i),q(E,eo+i)
        if TY(a) and TY(c): tmap[a][c]+=1; continue
        if typed and ES.get(et)==0x240 and i==0x1f0: continue  # rule shader vars: rebuilt
        pa,pe=ptr(lv,a),ptr(ev,c)
        if pa is not None and pe is not None:
            if not typed:  # arrays: walk each element pointer pair
                pass
            walk(L,lv,E,ev,pa,pe,seen,owner); continue
        if a!=c: diffs[(owner if not typed else lt, i if typed else -1)][(a,c)]+=1
def arraywalk(L,lv,E,ev,lo,eo,n,seen,owner):
    q=lambda x,o: struct.unpack_from('<Q',x,o)[0]
    for k in range(n):
        pa,pe=ptr(lv,q(L,lo+8*k)),ptr(ev,q(E,eo+8*k))
        if pa is not None and pe is not None: walk(L,lv,E,ev,pa,pe,seen,owner)
for rel in open('../yptall/pairs.txt').read().split():
    L,lv=load('../yptall/L/'+rel); E,ev=load('../yptall/E/'+rel)
    q=lambda x,o: struct.unpack_from('<Q',x,o)[0]
    for r in (0x38,0x48,0x50):
        dl,de=ptr(lv,q(L,r)),ptr(ev,q(E,r))
        n=q(L,dl+0x38)&0xffff
        arraywalk(L,lv,E,ev,ptr(lv,q(L,dl+0x30)),ptr(ev,q(E,de+0x30)),n,set(),None)
print('types')
for lt,c in sorted(tmap.items(), key=lambda t:-sum(t[1].values())): print(' ',hex(lt), [(hex(e),n) for e,n in c.most_common(3)])
print('diffs')
for (t,i),c in sorted(diffs.items(), key=lambda kv:-sum(kv[1].values()))[:50]:
    print(' ',hex(t) if t else None, hex(i), sum(c.values()), [(hex(a),hex(b),n) for (a,b),n in c.most_common(3)])
