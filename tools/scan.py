"""IDA-style pattern scan over a dumped image. usage: scan.py <dump> "<pattern>" ..."""
import re, sys
data = open(sys.argv[1], "rb").read()
def rx(p):
    return re.compile(b"".join(b"." if t == "?" else re.escape(bytes([int(t, 16)])) for t in p.split()), re.S)
for p in sys.argv[2:]:
    hits = [m.start() for m in rx(p).finditer(data)]
    print(f"{len(hits):3d} hit(s)  {p}  ->", [hex(h) for h in hits[:5]])
