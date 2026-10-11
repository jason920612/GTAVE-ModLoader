"""Research: find 8-byte pointers to given addresses (or into [address, address+range)) in the game's private memory.
usage: ptrscan.py <range> <hex address> [...]   (range in bytes, 0 = exact)"""
import sys
import numpy as np
sys.path.insert(0, __import__('os').path.dirname(__file__))
import memfind

pm = memfind.pm
span = int(sys.argv[1], 0)
targets = np.array(sorted(int(a, 16) for a in sys.argv[2:]), dtype=np.uint64)
for base, size in memfind.regions():
    try:
        data = pm.read_bytes(base, size)
    except Exception:
        continue
    q = np.frombuffer(data[: len(data) // 8 * 8], dtype=np.uint64)
    idx = np.searchsorted(targets, q, side='right') - 1
    ok = idx >= 0
    hit = np.zeros(len(q), dtype=bool)
    hit[ok] = (q[ok] - targets[idx[ok]]) <= np.uint64(span)
    for i in np.nonzero(hit)[0]:
        print(f'{base + 8 * int(i):#x} -> {int(q[i]):#x}')
