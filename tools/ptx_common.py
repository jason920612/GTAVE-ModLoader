import struct, sys, zlib, collections, glob
sys.path.insert(0, __import__("os").path.dirname(__file__))
from convert_ytd import block_size
def load(p):
    d=open(p,'rb').read(); a,b=struct.unpack_from('<II',d,8)
    x=zlib.decompressobj(-15).decompress(d[16:]); return x, block_size(a)
def ptr(vs,v):
    if v>>28==5 and v-0x50000000<vs: return v-0x50000000
    if v>>28==6: return vs+v-0x60000000
    return None
def objects(path):
    x,vs=load(path); q=lambda o: struct.unpack_from('<Q',x,o)[0] if o+8<=len(x) else 0
    seen=set(); stack=[0]; objs={}
    while stack:
        o=stack.pop()
        if o in seen or o>=len(x): continue
        seen.add(o)
        v=q(o)
        if 0x140000000<=v<0x150000000: objs[o]=v
        for i in range(0,0x200,8):
            p=ptr(vs,q(o+i))
            if p is not None and p not in seen: stack.append(p)
    return objs
if __name__=='__main__':
    for side in ('leg','enh'):
        c=collections.Counter()
        for f in glob.glob(side+'/*.ypt'):
            for o,v in objects(f).items(): c[v]+=1
        print(side, len(c)); 
        for v,n in sorted(c.items(), key=lambda t:-t[1]): print('  ',hex(v),n)
