"""Smallest distance to the next block per object type id, legacy vs Enhanced (writes sizes.json for ptx_pairwalk.py)."""
import struct, glob, collections, bisect
from ptx_common import load, ptr
TY=lambda v: 0x140000000<=v<0x150000000
def scan(side):
    sz=collections.defaultdict(list)
    for f in glob.glob('../yptall/'+('L' if side=='leg' else 'E')+'/*/*.ypt'):
        x,vs=load(f); q=lambda o: struct.unpack_from('<Q',x,o)[0] if o+8<=len(x) else 0
        # every pointer target is a block start
        starts=set([0])
        for o in range(0,len(x)-7,8):
            p=ptr(vs,q(o))
            if p is not None: starts.add(p)
        st=sorted(starts)
        for s in st:
            v=q(s)
            if TY(v):
                k=bisect.bisect_right(st,s)
                nxt=st[k] if k<len(st) else len(x)
                # stop at an embedded object of another type inside? keep simple
                sz[v].append(nxt-s)
    return {t:(len(v),min(v)) for t,v in sz.items()}
L,E=scan('leg'),scan('enh')
import json
json.dump({'L':{hex(k):v for k,v in L.items()},'E':{hex(k):v for k,v in E.items()}},open('sizes.json','w'))
lc={}; ec={}
for k,(n,m) in L.items(): lc.setdefault(n,[]).append((k,m))
for k,(n,m) in E.items(): ec.setdefault(n,[]).append((k,m))
for n in sorted(set(lc)|set(ec), reverse=True):
    print(n, [(hex(k),hex(m)) for k,m in lc.get(n,[])], '|', [(hex(k),hex(m)) for k,m in ec.get(n,[])])
