"""Research: find float triplets near a target in the game's private memory (research/phase0.md §42).
usage: floatscan.py <x> <y> <z> [tolerance=0.001] [max hits=40]
Scans 4-byte aligned positions; prints addresses where three consecutive floats match within the tolerance."""
import sys
import numpy as np
sys.path.insert(0, __import__('os').path.dirname(__file__))
import memfind

pm = memfind.pm
tx, ty, tz = (float(v) for v in sys.argv[1:4])
tol = float(sys.argv[4]) if len(sys.argv) > 4 else 1e-3
limit = int(sys.argv[5]) if len(sys.argv) > 5 else 40
hits = 0
for base, size in memfind.regions():
    try:
        data = pm.read_bytes(base, size)
    except Exception:
        continue
    f = np.frombuffer(data[: len(data) // 4 * 4], dtype=np.float32)
    with np.errstate(invalid='ignore'):
        m = (np.abs(f[:-2] - tx) < tol) & (np.abs(f[1:-1] - ty) < tol) & (np.abs(f[2:] - tz) < tol)
    for i in np.nonzero(m)[0]:
        print(f'{base + 4 * int(i):#x}  {f[i]:.5f} {f[i + 1]:.5f} {f[i + 2]:.5f}')
        hits += 1
        if hits >= limit:
            sys.exit(0)
