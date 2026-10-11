"""Fit collision capsules to a ped's body from its meshes (basketball physics bodies, research/phase0.md §46).
usage: fit_capsules.py <bball_skeleton.txt> <out.json> <plot.png> <component.ydd> [...]

- The skeleton file is the basketball mod's "skeleton" dump (rest local transforms; tools/skel_rest.py).
- Each drawable carries its own skeleton (drawable +0x18: +0x1A u16 bone count, +0x20 records of 0x50 with +0x38 name),
  and each geometry a table (+0x68, count +0x72) from blend index to that skeleton's bone. Vertices are in the model's
  rest space (the full skeleton's bind pose), so they are moved into each body bone's rest frame.
- Cloth vertices (jacket flaps: their blend data is the cloth simulation's, with 255 indices and weights not adding up
  to 255) are skipped: they are not body volume.
- Every vertex goes to the bone with its largest weight; helper bones (MH_, RB_, IK_...) count for their nearest SKEL_
  ancestor; bones are grouped into body segments (SEGMENTS).
- Torso segments (pelvis, abdomen, chest): a capsule across the body, radius from the front-back depth; the three stack
  up to cover the torso's height.
- Other segments: principal axis of the points (bone frame), radius = 85th percentile of the distance to the axis (loose
  cloth and stray vertices ignored), ends = 3rd / 97th percentile along the axis, pulled in by the radius.
"""
import json
import math
import struct
import sys

import numpy as np

sys.path.insert(0, __import__("os").path.dirname(__file__))
from skel_rest import load, qconj, qrot  # noqa: E402
from ydd_mesh import Resource, geometries, vertices  # noqa: E402

FINGERS = {f"SKEL_{s}_Finger{f}{j}": f"{s.lower()}_finger{f}{j}" for s in "LR" for f in range(5) for j in range(3)}
SEGMENTS = {
    "SKEL_ROOT": "pelvis", "SKEL_Pelvis": "pelvis",
    "SKEL_Spine_Root": "abdomen", "SKEL_Spine0": "abdomen", "SKEL_Spine1": "abdomen",
    "SKEL_Spine2": "chest", "SKEL_Spine3": "chest", "SKEL_L_Clavicle": "chest", "SKEL_R_Clavicle": "chest",
    "SKEL_Neck_1": "head", "SKEL_Head": "head",
    **{f"SKEL_{s}_{b}": f"{s.lower()}_{n}" for s in "LR" for b, n in (
        ("UpperArm", "upperarm"), ("Forearm", "forearm"), ("Hand", "hand"), ("Thigh", "thigh"), ("Calf", "calf"),
        ("Foot", "foot"), ("Toe0", "toe"))},
    **FINGERS,
    # Roll (twist) bones hang off the bone above the limb they twist.
    **{f"RB_{s}_{b}": f"{s.lower()}_{n}" for s in "LR" for b, n in (("ThighRoll", "thigh"), ("ArmRoll", "upperarm"), ("ForeArmRoll", "forearm"))},
}
# The bone whose frame a segment's capsule is expressed in.
FRAME = {}
for bone, seg in SEGMENTS.items():
    FRAME.setdefault(seg, bone)
FRAME.update({"pelvis": "SKEL_Pelvis", "abdomen": "SKEL_Spine0", "chest": "SKEL_Spine3", "head": "SKEL_Head"})


def drawable_skeleton(res, drawable):
    sk = res.u64(drawable + 0x18) & 0x0FFFFFFF
    count = struct.unpack_from("<H", res.v, sk + 0x1A)[0]
    recs = res.u64(sk + 0x20) & 0x0FFFFFFF
    names = []
    for i in range(count):
        n = res.u64(recs + 0x50 * i + 0x38) & 0x0FFFFFFF
        names.append(res.v[n:res.v.index(0, n)].decode("latin-1"))
    return names


def segment_of(name, bones, by_name):
    """Body segment of a skeleton bone (helper bones: their nearest mapped ancestor)."""
    i = by_name.get(name)
    while i is not None and i >= 0:
        if bones[i]["name"] in SEGMENTS:
            return SEGMENTS[bones[i]["name"]]
        i = bones[i]["parent"]
    return None


def fit(points):
    c = points.mean(axis=0)
    _, _, vt = np.linalg.svd(points - c, full_matrices=False)
    axis = vt[0]
    along = (points - c) @ axis
    radial = np.linalg.norm((points - c) - np.outer(along, axis), axis=1)
    r = float(np.percentile(radial, 85))
    lo, hi = float(np.percentile(along, 3)), float(np.percentile(along, 97))
    if hi - lo < 2 * r:  # a sphere
        mid = (lo + hi) / 2
        lo = hi = mid
    else:
        lo, hi = lo + r, hi - r
    return c + axis * lo, c + axis * hi, r


def fit_across(points, lateral, depth):
    """Torso segments: a capsule across the body (the torso is wide and shallow). Centred on the middle of the
    front-back and vertical ranges (meshes are denser in front), radius half the front-back depth."""
    up = np.cross(lateral, depth)
    mid = lambda v: (np.percentile(v, 3) + np.percentile(v, 97)) / 2  # noqa: E731
    # Across the body it is symmetric about the bone (on the midline): half width = mean of the two sides.
    c = depth * mid(points @ depth) + up * mid(points @ up)
    r = float((np.percentile(points @ depth, 97) - np.percentile(points @ depth, 3)) / 2)
    along = points @ lateral
    half = (abs(float(np.percentile(along, 3))) + abs(float(np.percentile(along, 97)))) / 2 - r
    lo, hi = (-half, half) if half > 0 else (0.0, 0.0)
    return c + lateral * lo, c + lateral * hi, r


def distance_to_capsule(p, a, b, r):
    ab = b - a
    t = np.clip(((p - a) @ ab) / max(ab @ ab, 1e-12), 0, 1)
    return np.linalg.norm(p - (a + np.outer(t, ab)), axis=1) - r


def main():
    skel_path, out_path, plot_path, files = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
    bones = load(skel_path)
    by_name = {b["name"]: i for i, b in enumerate(bones)}
    points = {}  # segment -> list of model-space points
    model_pts, model_seg = [], []
    for f in files:
        res = Resource(f)
        root_list = res.u64(0x30) & 0x0FFFFFFF
        for k in range(struct.unpack_from("<H", res.v, 0x38)[0]):
            drawable = res.u64(root_list + 8 * k) & 0x0FFFFFFF
            names = drawable_skeleton(res, drawable)
            for g in geometries(res):
                vs = vertices(res, g)
                odd = lambda iw: abs(sum(w for _, w in iw) - 255) > 2 or any(w and j >= len(g["bones"]) for j, w in iw)  # noqa: E731
                n = sum(1 for _, iw in vs if odd(iw))
                if n:
                    print(f"{f}: geometry @{g['at']:#x}: {n} of {len(vs)} vertices are cloth (simulation data), skipped")
                vs = [(p, iw) for p, iw in vs if not odd(iw)]
                for p, iw in vs:
                    j, w = max(iw, key=lambda t: t[1])
                    if w == 0 or j >= len(g["bones"]) or g["bones"][j] >= len(names):
                        continue
                    seg = segment_of(names[g["bones"][j]], bones, by_name)
                    if seg:
                        points.setdefault(seg, []).append(p)
                        model_pts.append(p)
                        model_seg.append(seg)
    out = {}
    for seg, pts in sorted(points.items()):
        frame = bones[by_name[FRAME[seg]]]
        local = np.array([qrot(qconj(frame["Q"]), (p[0] - frame["T"][0], p[1] - frame["T"][1], p[2] - frame["T"][2])) for p in pts])
        if len(local) < 8:
            continue
        if seg in ("pelvis", "abdomen", "chest"):
            # The model faces -y here (the feet point that way); x is across the body. Stacked bands cover the height.
            lateral = np.array(qrot(qconj(frame["Q"]), (1, 0, 0)))
            depth = np.array(qrot(qconj(frame["Q"]), (0, 1, 0)))
            up = np.array(qrot(qconj(frame["Q"]), (0, 0, 1)))
            h = local @ up
            lo, hi = np.percentile(h, 3), np.percentile(h, 97)
            r0 = float((np.percentile(local @ depth, 97) - np.percentile(local @ depth, 3)) / 2)
            bands = max(1, int(math.ceil((hi - lo) / (1.6 * r0))))
            edges = np.linspace(lo, hi, bands + 1)
            caps = []
            for k in range(bands):
                sel = (h >= edges[k] - 1e-6) & (h <= edges[k + 1] + 1e-6)
                if sel.sum() >= 8:
                    caps.append(fit_across(local[sel], lateral, depth))
        else:
            caps = [fit(local)]
        d = np.min([distance_to_capsule(local, a, b, r) for a, b, r in caps], axis=0)
        out[seg] = {"bone": FRAME[seg], "capsules": [{"a": [round(float(x), 4) for x in a], "b": [round(float(x), 4) for x in b], "radius": round(r, 4)}
                                                     for a, b, r in caps],
                    "vertices": len(local), "inside": round(float((d <= 0.01).mean()), 3), "mean_outside": round(float(np.clip(d, 0, None).mean()), 4)}
        print(f"{seg:14s} {FRAME[seg]:18s} n={len(local):5d} capsules={len(caps)} r={'/'.join(f'{c[2]:.3f}' for c in caps)} "
              f"inside(+1cm)={out[seg]['inside']:.2f} mean outside={out[seg]['mean_outside']:.4f}")
    json.dump({"skeleton": "player_two", "segments": out}, open(out_path, "w"), indent=1)
    plot(bones, by_name, out, np.array(model_pts), model_seg, plot_path)


def plot(bones, by_name, caps, pts, segs, path):
    import matplotlib
    matplotlib.use("Agg")
    import matplotlib.pyplot as plt
    names = sorted(set(segs))
    colours = {s: plt.cm.tab20(i % 20) for i, s in enumerate(names)}
    fig, axes = plt.subplots(1, 3, figsize=(21, 10))
    views = [(0, 2, "front (x, z)", None), (1, 2, "side (y, z)", None), (0, 2, "right hand (x, z)", (-0.65, -0.4, -0.2, 0.08))]
    sel = np.random.default_rng(0).choice(len(pts), min(len(pts), 25000), replace=False)
    for ax, (i, j, title, box) in zip(axes, views):
        ax.scatter(pts[sel, i], pts[sel, j], s=0.3, c=[colours[segs[k]] for k in sel], alpha=0.35)
        for seg, entry in caps.items():
            fr = bones[by_name[entry["bone"]]]
            for c in entry["capsules"]:
                a = np.array(qrot(fr["Q"], c["a"])) + fr["T"]
                b = np.array(qrot(fr["Q"], c["b"])) + fr["T"]
                r = c["radius"]
                d = np.array([b[i] - a[i], b[j] - a[j]])
                n = np.array([-d[1], d[0]]) / (np.linalg.norm(d) or 1) * r
                for s in (1, -1):
                    ax.plot([a[i] + s * n[0], b[i] + s * n[0]], [a[j] + s * n[1], b[j] + s * n[1]], "k-", lw=0.6)
                for e in (a, b):
                    ax.add_patch(plt.Circle((e[i], e[j]), r, fill=False, lw=0.6, color="k"))
        ax.set_aspect("equal")
        ax.set_title(title)
        if box:
            ax.set_xlim(box[0], box[1])
            ax.set_ylim(box[2], box[3])
    plt.tight_layout()
    plt.savefig(path, dpi=110)


if __name__ == "__main__":
    main()
