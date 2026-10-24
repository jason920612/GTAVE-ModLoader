"""The graphics mod's hydrology (research/phase0.md §60.5): the Alamo Sea gets the inflows its terrain implies.

The game's Alamo Sea has no river running into it, yet two running out (Cassidy Creek, the Zancudo River). Its
terrain drains 7 km2 into it: the Grand Senora desert from the south, Mount Chiliad's slopes from the north. Each
inflow here starts where that drainage meets the lake; it is followed upstream along the branch carrying the most
water (the ground's drainage: a priority flood from the lake) to where its catchment gets small (its spring), then
laid on the valley floor: 2 m apart a cross-section with the water's level (the bed filled downstream, plus the
depth its catchment gives it), how far it reaches each side (where the bank rises above it, or as wide as the
stream gets), and its speed (Manning's formula, from depth and slope). The ground is the pack's own (the HD terrain
meshes as the renderer draws them), so the water lies in it exactly.

Writes <out> (JSON: per stream its cross-sections), which `ue5export pack --streams <out>` turns into models and
entities of the pack; with --png <folder>, a map and a long profile of each stream.
usage: python -I ue5hydrology.py <pack folder> <out .json> [--png <folder>]"""
import heapq, json, math, os, struct, sys, time
import numpy as np
from scipy import ndimage

# The inflows: where each meets the lake (the terrain's drainage, §60.5), the catchment its spring has, how far away
# the renderer still draws it (m).
STREAMS = [
    ("senora_river", (968.0, 3668.0), 0.05, 1500.0),
    ("chiliad_east_creek", (1848.0, 4572.0), 0.05, 900.0),
    ("chiliad_south_creek", (922.0, 4286.0), 0.05, 900.0),
]
LAKE = 30.0                               # the Alamo Sea's level (water.xml)
MAP = (-3800.0, -4200.0, 4400.0, 8000.0)  # x0, y0, x1, y1: the whole map (its drainage ends only in the sea or a lake)
COARSE, FINE, STEP = 4.0, 0.5, 2.0        # m: the drainage's cells, the cross-sections' cells, their spacing
MANNING_N = 0.045                         # a natural channel with stones and some growth

args = sys.argv[1:]
PNG = args[args.index("--png") + 1] if "--png" in args else None
args = [a for i, a in enumerate(args) if a != "--png" and (i == 0 or args[i - 1] != "--png")]
PACK, OUT = args[0], args[1]


def table(path):
    f = open(path, "rb")
    magic, kind, version, count, off = struct.unpack("<4sIIIQ", f.read(24))
    f.seek(off)
    return f, {k: (o, s) for k, o, s, fl in (struct.unpack("<QQII", f.read(24)) for _ in range(count))}


def rotate(q, v):
    x, y, z, w = q
    u = np.array([x, y, z])
    t = 2 * np.cross(u, v)
    return v + w * t + np.cross(u, t)


class Pack:
    """The pack's HD entities' meshes: terrain (material kind 4) and other opaque ones (0: roads, yards, buildings)."""

    def __init__(self):
        self.mf, self.mt = table(os.path.join(PACK, "models.gpk"))
        wf, wt = table(os.path.join(PACK, "world.gpk"))
        o, s = wt[4]
        wf.seek(o)
        ents = np.frombuffer(wf.read(s), dtype=np.dtype([("arch", "<u4"), ("ymap", "<u4"), ("pos", "<f4", 3), ("rot", "<f4", 4), ("sxy", "<f4"),
                                                          ("sz", "<f4"), ("lod", "<f4", 2), ("parent", "<u4"), ("flags", "<u4"), ("tint", "<u4"),
                                                          ("interior", "<u4"), ("lodLevel", "u1"), ("prio", "u1"), ("children", "<u2")]))
        self.ents = ents[np.isin(ents["lodLevel"], (0, 5))]
        self.gx, self.gy = self.ents["pos"][:, 0] / 100, -self.ents["pos"][:, 1] / 100
        self.cache = {}

    def sections(self, arch):
        if arch not in self.cache:
            self.cache[arch] = []
            entry = self.mt.get(arch)
            if entry:
                self.mf.seek(entry[0])
                data = self.mf.read(entry[1])
                mcount, scount = struct.unpack_from("<HH", data, 64)
                kinds = [data[76 + 200 * i + 5] for i in range(mcount)]
                if 0 in kinds or 4 in kinds:
                    at = 76 + 200 * mcount
                    for _ in range(scount):
                        mat, uvc, sflags, vc, ic, lod = struct.unpack_from("<HBBIIB", data, at)
                        at += 16
                        pos = np.frombuffer(data, np.float32, vc * 3, at).reshape(vc, 3)
                        at += 12 * vc + sum(4 * vc for b in (1, 2, 4, 8) if sflags & b) + 8 * vc * uvc
                        isz = 4 if sflags & 16 else 2
                        idx = np.frombuffer(data, np.uint32 if isz == 4 else np.uint16, ic, at).astype(np.int64).reshape(-1, 3)
                        at += ic * isz
                        at += (4 - at % 4) % 4
                        if lod == 0 and kinds[mat] in (0, 4):
                            self.cache[arch].append((kinds[mat], pos.copy(), idx))
        return self.cache[arch]

    def triangles(self, kind, box=None, batch=4_000_000):
        """Batches of the triangles of that kind (n, 3, 3; the game's metres) of the entities placed in box (and 400 m
        about it: a terrain tile reaches that far from its origin)."""
        pick = np.ones(len(self.ents), bool)
        if box is not None:
            pick = (self.gx > box[0] - 400) & (self.gx < box[2] + 400) & (self.gy > box[1] - 400) & (self.gy < box[3] + 400)
        out, count = [], 0
        for e in np.nonzero(pick)[0]:
            ent = self.ents[e]
            for k, pos, idx in self.sections(int(ent["arch"])):
                if k != kind:
                    continue
                local = pos * np.array([ent["sxy"], ent["sxy"], ent["sz"]])
                world = rotate(ent["rot"], local) + ent["pos"]
                g = np.stack([world[:, 0] / 100, -world[:, 1] / 100, world[:, 2] / 100], 1).astype(np.float32)
                tris = g[idx]
                if box is not None:
                    tris = in_box(tris, *box)
                out.append(tris)
                count += len(tris)
                if count >= batch:
                    yield np.concatenate(out)
                    out, count = [], 0
        if out:
            yield np.concatenate(out)


def rasterize(tris, x0, y0, cell, nx, ny, out, cap=None):
    """Each cell centre's height on the triangles over it, the highest (with cap: only heights up to cap there)."""
    if len(tris) == 0:
        return
    a, b, c = tris[:, 0].astype(np.float64), tris[:, 1].astype(np.float64), tris[:, 2].astype(np.float64)
    lo = np.minimum(np.minimum(a, b), c)
    hi = np.maximum(np.maximum(a, b), c)
    i0 = np.ceil((lo[:, 0] - x0) / cell - 0.5).astype(np.int64)
    i1 = np.floor((hi[:, 0] - x0) / cell - 0.5).astype(np.int64)
    j0 = np.ceil((lo[:, 1] - y0) / cell - 0.5).astype(np.int64)
    j1 = np.floor((hi[:, 1] - y0) / cell - 0.5).astype(np.int64)
    i0, j0 = np.maximum(i0, 0), np.maximum(j0, 0)
    i1, j1 = np.minimum(i1, nx - 1), np.minimum(j1, ny - 1)
    w, h = i1 - i0 + 1, j1 - j0 + 1
    d = (b[:, 1] - c[:, 1]) * (a[:, 0] - c[:, 0]) + (c[:, 0] - b[:, 0]) * (a[:, 1] - c[:, 1])
    keep = (w > 0) & (h > 0) & (np.abs(d) > 1e-9)
    size = np.maximum(w, h)
    for bucket in (1, 2, 4, 8, 16, 32, 64, 128, 256):
        sel = np.nonzero(keep & (size <= bucket) & (size > bucket // 2))[0]
        for chunk in range(0, len(sel), max(1, 2_000_000 // (bucket * bucket))):
            t = sel[chunk:chunk + max(1, 2_000_000 // (bucket * bucket))]
            di, dj = np.meshgrid(np.arange(bucket), np.arange(bucket))
            di, dj = di.ravel()[None, :], dj.ravel()[None, :]
            ii, jj = i0[t, None] + di, j0[t, None] + dj
            ok = (di < w[t, None]) & (dj < h[t, None])
            px, py = x0 + (ii + 0.5) * cell, y0 + (jj + 0.5) * cell
            at, bt, ct, dt = a[t], b[t], c[t], d[t, None]
            l1 = ((bt[:, 1, None] - ct[:, 1, None]) * (px - ct[:, 0, None]) + (ct[:, 0, None] - bt[:, 0, None]) * (py - ct[:, 1, None])) / dt
            l2 = ((ct[:, 1, None] - at[:, 1, None]) * (px - ct[:, 0, None]) + (at[:, 0, None] - ct[:, 0, None]) * (py - ct[:, 1, None])) / dt
            l3 = 1 - l1 - l2
            ok &= (l1 >= -1e-6) & (l2 >= -1e-6) & (l3 >= -1e-6)
            z = l1 * at[:, 2, None] + l2 * bt[:, 2, None] + l3 * ct[:, 2, None]
            jj, ii, z = jj[ok], ii[ok], z[ok]
            if cap is not None:
                limit = cap[jj, ii]
                good = np.isnan(limit) | (z <= limit)
                jj, ii, z = jj[good], ii[good], z[good]
            np.maximum.at(out, (jj, ii), z)


def ground(terrain, x0, y0, cell, nx, ny):
    """The valley floor: the terrain; where it has holes (under a road, a town's yards), the terrain about them (a
    road's embankment is no dam: water passes under it, as through a culvert; the road's deck covers it there)."""
    t = np.full((ny, nx), -np.inf)
    rasterize(terrain, x0, y0, cell, nx, ny, t)
    hole = ~np.isfinite(t)
    if hole.any() and (~hole).any():
        _, (jj, ii) = ndimage.distance_transform_edt(hole, return_indices=True)
        t = t[jj, ii]
    return t


def built(pack, box, t, x0, y0, cell, nx, ny):
    """Where something else lies on the ground (a road, a yard, a building's floor: an opaque surface within 0.6 m
    below and 3 m above the terrain): no stream runs there (it runs beside a road, crosses it under it)."""
    top = np.full((ny, nx), -np.inf)
    for tris in pack.triangles(0, box):
        rasterize(tris, x0, y0, cell, nx, ny, top, cap=t + 3.0)
    return top >= t - 0.6


def in_box(tris, x0, y0, x1, y1):
    lo = tris.min(axis=1)
    hi = tris.max(axis=1)
    return tris[(hi[:, 0] >= x0) & (lo[:, 0] <= x1) & (hi[:, 1] >= y0) & (lo[:, 1] <= y1)]


def water_quads():
    """water.xml's quads (the pack's copy): min x, min y, max x, max y, level."""
    wf, wt = table(os.path.join(PACK, "world.gpk"))
    o, s = wt[15]
    wf.seek(o)
    data = wf.read(s)
    return [struct.unpack_from("<5f", data, k) for k in range(0, len(data), 28)]


t0 = time.time()
pack = Pack()
terrain = np.concatenate(list(pack.triangles(4)))
print(f"{len(terrain)} terrain triangles ({time.time() - t0:.0f} s)", flush=True)

# The map's drainage (coarse): priority flood from its water - the sea, the Alamo Sea, the reservoirs (water.xml's
# quads where the ground is below them or there is none) and the open sea about the map (its edge). A hole in the
# terrain on land (under a road: the roads' holes reach the coast) is land: the terrain about it.
x0, y0, x1, y1 = MAP
nx, ny = int((x1 - x0) / COARSE), int((y1 - y0) / COARSE)
g = np.full((ny, nx), -np.inf)
rasterize(terrain, x0, y0, COARSE, nx, ny, g)
hole = ~np.isfinite(g)
xs = x0 + (np.arange(nx) + 0.5) * COARSE
ys = y0 + (np.arange(ny) + 0.5) * COARSE
water = np.zeros(g.shape, bool)
lake = np.zeros(g.shape, bool)
for qx0, qy0, qx1, qy1, level in water_quads():
    i0, i1 = np.searchsorted(xs, qx0), np.searchsorted(xs, qx1)
    j0, j1 = np.searchsorted(ys, qy0), np.searchsorted(ys, qy1)
    under = hole[j0:j1, i0:i1] | (g[j0:j1, i0:i1] < level)
    water[j0:j1, i0:i1] |= under
    if abs(level - LAKE) < 0.01:
        lake[j0:j1, i0:i1] |= under
water[[0, -1], :] = True
water[:, [0, -1]] = True
_, (jj, ii) = ndimage.distance_transform_edt(hole, return_indices=True)
g = g[jj, ii]  # (holes inland: the terrain about them)
# (about the Alamo Sea: what is built on the ground counts 1.5 m higher, the water finds its way beside it)
CATCH = (-700.0, 800.0, 3000.0, 6200.0)
on_ground = built(pack, CATCH, g, x0, y0, COARSE, nx, ny)
outlet = water
filled = g + 1.5 * on_ground
to = np.full(g.size, -1, np.int64)
done = outlet.ravel().copy()
heap = [(filled.flat[k], int(k)) for k in np.nonzero(done)[0]]
heapq.heapify(heap)
N8 = [(-1, -1), (-1, 0), (-1, 1), (0, -1), (0, 1), (1, -1), (1, 0), (1, 1)]
found = []  # (the order cells are reached in: each drains to one reached before it)
while heap:
    hgt, k = heapq.heappop(heap)
    j, i = divmod(k, nx)
    for dj, di in N8:
        jj, ii = j + dj, i + di
        if 0 <= jj < ny and 0 <= ii < nx:
            kk = jj * nx + ii
            if not done[kk]:
                done[kk] = True
                to[kk] = k
                found.append(kk)
                if filled.flat[kk] < hgt:
                    filled.flat[kk] = hgt + 1e-4
                heapq.heappush(heap, (filled.flat[kk], kk))
acc = np.ones(g.size)
for k in reversed(found):
    acc[to[k]] += acc[k]
km2 = COARSE * COARSE / 1e6
up_order = np.argsort(to)
up_from = np.searchsorted(to[up_order], np.arange(g.size))
up_to = np.searchsorted(to[up_order], np.arange(g.size), side="right")
print(f"drainage of the map: {nx}x{ny} cells ({time.time() - t0:.0f} s)", flush=True)
if "--dump" in sys.argv:
    np.savez_compressed(os.path.join(os.path.dirname(os.path.abspath(OUT)), "ue5hydrology_drainage.npz"), g=g, filled=filled, to=to, acc=acc,
                        water=water, lake=lake, x0=x0, y0=y0, cell=COARSE)


def smooth(v, sigma):
    return ndimage.gaussian_filter1d(v, sigma, mode="nearest")


def resample(px, py, step):
    s = np.concatenate([[0], np.cumsum(np.hypot(np.diff(px), np.diff(py)))])
    at = np.arange(0, s[-1], step)
    return np.interp(at, s, px), np.interp(at, s, py), at


streams = []
for name, (mx, my), least, lod_distance in STREAMS:
    # The mouth: of the land cells draining straight into the lake near it, the one with the most water.
    near = [k for k in np.nonzero(~lake.ravel() & (to >= 0))[0] if lake.flat[to[k]]]
    if name == STREAMS[0][0]:
        for k in sorted(near, key=lambda k: -acc[k])[:8]:
            print(f"  (into the lake at ({xs[k % nx]:6.0f}, {ys[k // nx]:6.0f}): {acc[k] * km2:5.2f} km2)")
    near = [k for k in near if math.hypot(xs[k % nx] - mx, ys[k // nx] - my) < 60]
    k = max(near, key=lambda k: acc[k])
    print(f"  {name}: mouth ({xs[k % nx]:.0f}, {ys[k // nx]:.0f}), {acc[k] * km2:.2f} km2", flush=True)
    path = [k]
    while True:
        ups = up_order[up_from[path[-1]]:up_to[path[-1]]]
        if len(ups) == 0:
            break
        best = ups[np.argmax(acc[ups])]
        if acc[best] * km2 < least:
            break
        path.append(best)
    path = path[::-1]  # spring -> mouth
    cx, cy = xs[np.array(path) % nx], ys[np.array(path) // nx]
    area_raw = acc[np.array(path)] * km2
    # Smoothed (the cells' steps out), 2 m apart, then put on the valley floor by the fine ground.
    px, py, along = resample(smooth(cx, 2.0), smooth(cy, 2.0), STEP)
    area = np.interp(along, np.concatenate([[0], np.cumsum(np.hypot(np.diff(cx), np.diff(cy)))]), area_raw)
    bx0, by0, bx1, by1 = px.min() - 30, py.min() - 30, px.max() + 30, py.max() + 30
    fnx, fny = int((bx1 - bx0) / FINE), int((by1 - by0) / FINE)
    if fnx * fny > 40_000_000:
        raise SystemExit(f"{name}: too large a box for the fine ground")
    fine = ground(in_box(terrain, bx0, by0, bx1, by1), bx0, by0, FINE, fnx, fny)
    fine_built = built(pack, (bx0, by0, bx1, by1), fine, bx0, by0, FINE, fnx, fny)
    kept = ndimage.binary_dilation(fine_built, iterations=3)  # (a verge of 1.5 m between the water and a road)
    raised = fine + 1.5 * kept  # (for finding the valley floor: beside a road, not on it)
    walls = np.where(kept, fine + 100.0, fine)  # (for the banks: the water stops short of a road's edge)

    def at(x, y, grid=fine):
        x, y = np.asarray(x, np.float64), np.asarray(y, np.float64)
        flat = ndimage.map_coordinates(grid, [np.atleast_1d(y).ravel() / FINE - by0 / FINE - 0.5, np.atleast_1d(x).ravel() / FINE - bx0 / FINE - 0.5],
                                       order=1, mode="nearest")
        return flat.reshape(x.shape)

    for _ in range(2):
        tx, ty = np.gradient(px), np.gradient(py)
        n = np.hypot(tx, ty)
        tx, ty = tx / n, ty / n
        nxv, nyv = -ty, tx  # (the left)
        offs = np.arange(-4.0, 4.01, 0.25)
        across = at(px[:, None] + nxv[:, None] * offs[None, :], py[:, None] + nyv[:, None] * offs[None, :], raised)
        shift = smooth(offs[np.argmin(across, axis=1)], 3.0)
        px, py = px + nxv * shift * 0.8, py + nyv * shift * 0.8
        px, py, along = resample(smooth(px, 2.0), smooth(py, 2.0), STEP)
        area = np.interp(np.linspace(0, 1, len(px)), np.linspace(0, 1, len(area)), area)
    tx, ty = np.gradient(px), np.gradient(py)
    n = np.hypot(tx, ty)
    tx, ty = tx / n, ty / n
    nxv, nyv = -ty, tx
    # The bed (the lowest across the middle), made to fall all the way with the least earth moved (a non-increasing
    # fit, absolute deviations: pool adjacent violators, block medians): a hollow fills (a pool) where filling it is
    # less than cutting its lip, a short rise across the way (a road's embankment, a rock lip) is cut where that is
    # less (a culvert: the water goes under, out of sight there).
    bed = at(px[:, None] + nxv[:, None] * np.linspace(-1, 1, 9)[None, :], py[:, None] + nyv[:, None] * np.linspace(-1, 1, 9)[None, :]).min(axis=1)
    blocks = []  # (mouth -> spring: values must not fall) [sorted values, count]
    for v in bed[::-1]:
        blocks.append([[float(v)], 1])
        while len(blocks) > 1 and blocks[-2][0][len(blocks[-2][0]) // 2] > blocks[-1][0][len(blocks[-1][0]) // 2]:
            last = blocks.pop()
            blocks[-1] = [sorted(blocks[-1][0] + last[0]), blocks[-1][1] + last[1]]
    fill = np.concatenate([np.full(n, vals[len(vals) // 2]) for vals, n in blocks])[::-1]
    culverts = int(np.sum(np.diff(np.concatenate([[0], (bed - fill > 0.3).astype(int)])) == 1))
    # (a hollow on the way the river fills and runs on from: a pond on its course, as rivers have them)
    # How big the stream is where its catchment is that big (hydraulic geometry of small semi-arid streams).
    root = np.sqrt(np.maximum(area, 1e-4))
    depth = np.clip(0.10 + 0.20 * root, 0.08, 0.50)
    half = np.clip(0.6 + 1.9 * root, 0.6, 3.8)
    spring = np.clip(along / 30.0, 0, 1)
    spring = spring * spring * (3 - 2 * spring)
    depth *= 0.4 + 0.6 * spring
    half *= 0.5 + 0.5 * spring
    level = fill + depth
    # Into the lake: the last metres level with it (a hair under, the lake's own surface covers the seam).
    end = len(level)
    for i in range(len(level)):
        if bed[i] < LAKE - 0.3:
            end = min(len(level), i + 3)
            break
    level = np.maximum(level, LAKE - 0.02)[:end]
    px, py, tx, ty, nxv, nyv, along, bed, fill, depth, half, area = (v[:end] for v in (px, py, tx, ty, nxv, nyv, along, bed, fill, depth, half, area))
    # Each side: out to where the bank rises above the water, at most a little wider than the stream gets (a pool
    # half again); no wider than the bend lets the inner side be.
    heading = np.unwrap(np.arctan2(ty, tx))
    curvature = smooth(np.gradient(heading) / STEP, 2.0)
    ts = np.arange(0.25, 6.01, 0.25)
    sides = []
    for sign in (1, -1):
        gx, gy = px[:, None] + sign * nxv[:, None] * ts[None, :], py[:, None] + sign * nyv[:, None] * ts[None, :]
        above = at(gx, gy, walls) >= level[:, None]
        cap = half * np.where(fill - bed > 0.3, 1.5, 1.0)
        inner = sign * curvature > 0
        cap = np.where(inner, np.minimum(cap, 0.8 / np.maximum(np.abs(curvature), 1e-3)), cap)
        first = np.where(above.any(axis=1), ts[np.argmax(above, axis=1)], np.inf)
        reach = np.minimum(first, cap)
        reach = smooth(ndimage.median_filter(reach, 5, mode="nearest"), 1.5)
        banked = first <= cap + 0.25
        sides.append((reach, banked))
    (left, lbank), (right, rbank) = sides
    # The cross-section's points, left to right (looking downstream): the left bank, half way, the middle, half way,
    # the right bank (buried 0.3 m in a bank that rises above the water; where none does, laid on the ground at the
    # stream's edge).
    rows = []
    slope = np.clip(-np.gradient(smooth(level, 3.0)) / STEP, 0.0, 2.0)
    speed = np.clip((1.0 / MANNING_N) * depth ** (2.0 / 3.0) * np.sqrt(np.maximum(slope, 0.0005)), 0.15, 3.0)
    for i in range(len(px)):
        offsets = [left[i] + (0.3 if lbank[i] else 0.0), left[i] / 2, 0.0, -right[i] / 2, -(right[i] + (0.3 if rbank[i] else 0.0))]
        zs = []
        for c, o in enumerate(offsets):
            # (the water's level across; an edge with no bank above the water there - as wide as the stream gets -
            # comes down to the ground, the water thinning out to nothing)
            banked = lbank[i] if c == 0 else rbank[i] if c == 4 else True
            gz = float(at(px[i] + nxv[i] * o, py[i] + nyv[i] * o))
            zs.append(round(float(level[i]) if banked else min(float(level[i]), gz + 0.02), 3))
        rows.append([round(float(px[i]), 3), round(float(py[i]), 3), round(float(tx[i]), 4), round(float(ty[i]), 4),
                     [round(o, 3) for o in offsets], zs, round(float(speed[i]), 3)])
    on_built = at(px, py, fine_built.astype(np.float64)) > 0.5
    fall = level[0] - level[-1]
    print(f"  {name}: {np.mean(on_built) * 100:.0f}% of the way on built ground ({np.sum(on_built) * STEP:.0f} m: crossings, culverts)", flush=True)
    print(f"{name}: {along[-1]:.0f} m from its spring ({px[0]:.0f}, {py[0]:.0f}, {level[0]:.1f} m) to the lake, falls {fall:.0f} m; "
          f"{area[-1]:.2f} km2 at the mouth; {(right + left).mean():.1f} m wide, {depth.mean():.2f} m deep on average; "
          f"pools {np.sum(fill - bed > 0.3) * STEP:.0f} m, the deepest {np.max(fill - bed):.1f} m; {culverts} culverts; speed {speed.min():.2f}-{speed.max():.2f} m/s", flush=True)
    streams.append({"name": name, "lodDistance": lod_distance, "sections": rows})
    if PNG:
        from PIL import Image, ImageDraw
        os.makedirs(PNG, exist_ok=True)
        shade = np.gradient(fine, axis=1) * -0.6 + np.gradient(fine, axis=0) * 0.6
        img = np.clip(0.55 + shade, 0, 1)
        rgb = np.stack([img * 0.85, img * 0.8, img * 0.65], -1)
        rgb[fine_built] = rgb[fine_built] * np.array([1.0, 0.55, 0.55])
        im = Image.fromarray((np.flipud(rgb) * 255).astype(np.uint8))
        scale = 2400 / max(im.size)
        im = im.resize((int(im.size[0] * scale), int(im.size[1] * scale)))
        dr = ImageDraw.Draw(im)
        to_px = lambda qx, qy: ((qx - bx0) / FINE * scale, (fny - (qy - by0) / FINE) * scale)
        for o, colour in ((left, (30, 80, 230)), (-right, (30, 80, 230)), (0 * left, (230, 30, 30))):
            dr.line([to_px(px[i] + nxv[i] * o[i], py[i] + nyv[i] * o[i]) for i in range(len(px))], fill=colour, width=2)
        for k in range(0, len(px), 250):
            dr.text(to_px(px[k], py[k]), f"{along[k]:.0f} m", fill=(0, 0, 0))
        im.save(os.path.join(PNG, f"{name}_map.png"))
        prof = Image.new("RGB", (1600, 500), (255, 255, 255))
        dr = ImageDraw.Draw(prof)
        zmin, zmax = float(bed.min()) - 1, float(level.max()) + 1
        X = lambda s: 20 + s / along[-1] * 1560
        Y = lambda z: 480 - (z - zmin) / (zmax - zmin) * 460
        dr.line([(X(s), Y(z)) for s, z in zip(along, bed)], fill=(120, 80, 40), width=2)
        dr.line([(X(s), Y(z)) for s, z in zip(along, level)], fill=(40, 90, 220), width=2)
        dr.text((24, 6), f"{name}: bed (brown), water (blue); {zmin:.0f}..{zmax:.0f} m over {along[-1]:.0f} m", fill=(0, 0, 0))
        prof.save(os.path.join(PNG, f"{name}_profile.png"))
os.makedirs(os.path.dirname(os.path.abspath(OUT)), exist_ok=True)
json.dump({"version": 1, "about": "Inflows of the Alamo Sea (tools/ue5hydrology.py): per section x, y, tangent x, y (the game's metres, "
           "downstream), offsets of its 5 points (left to right) along the left normal (-ty, tx), their heights, the water's speed (m/s)",
           "streams": streams}, open(OUT, "w"), separators=(",", ":"))
print(f"wrote {OUT} ({time.time() - t0:.0f} s)")
