"""Game script (.ysc) parsing: decompressed resource -> program header, code, natives, strings, statics.
usage: ysc.py <file.ysc>  (prints the header words)"""
import struct, zlib, sys
sys.path.insert(0, __import__("os").path.dirname(__file__))
from convert_ytd import block_size
def load(p):
    d=open(p,'rb').read()
    a,b=struct.unpack_from('<II',d,8)
    x=zlib.decompressobj(-15).decompress(d[16:])
    return x, block_size(a)
def ptr(v, vs):
    if v>>28==5: return v-0x50000000
    if v>>28==6: return vs+v-0x60000000
    return None
if __name__=="__main__":
    x,vs=load(sys.argv[1])
    print(len(x), hex(vs))
    for o in range(0,0x80,8):
        v=struct.unpack_from('<Q',x,o)[0]; print(hex(o), hex(v))

class Prog:
    def __init__(s, path):
        x,vs=load(path); s.x=x; s.vs=vs
        q=lambda o: struct.unpack_from('<Q',x,o)[0]
        u=lambda o: struct.unpack_from('<I',x,o)[0]
        s.codesize=u(0x1c); s.nstatics=u(0x24); s.nnatives=u(0x2c)
        s.name=x[ptr(q(0x60),vs):].split(b'\0')[0].decode()
        cp=ptr(q(0x10),vs); s.code=bytearray()
        for i in range((s.codesize+0x3fff)//0x4000):
            p=ptr(q(cp+8*i),vs); s.code+=x[p:p+min(0x4000,s.codesize-i*0x4000)]
        np_=ptr(q(0x40),vs); s.natives=[q(np_+8*i) for i in range(s.nnatives)]
        s.strsize=u(0x70); sp=ptr(q(0x68),vs); s.strings=bytearray()
        for i in range((s.strsize+0x3fff)//0x4000):
            p=ptr(q(sp+8*i),vs); s.strings+=x[p:p+min(0x4000,s.strsize-i*0x4000)]
        st=ptr(q(0x30),vs); s.statics=[q(st+8*i) for i in range(s.nstatics)] if st is not None else []
