"""Rest (bind) pose of a ped skeleton dumped by the basketball mod's "skeleton" command (research aid).
usage: skel_rest.py <bball_skeleton.txt> [bone name ...]

Each line "bone i name parent qx qy qz qw tx ty tz" is a bone's rest local transform (parent space). Object
transforms are the product down the parent chain: p_object = R_parent * p_local + t_parent.
"""
import sys


def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def qrot(q, v):
    x, y, z, w = q
    ux, uy, uz = x, y, z
    tx, ty, tz = 2 * (uy * v[2] - uz * v[1]), 2 * (uz * v[0] - ux * v[2]), 2 * (ux * v[1] - uy * v[0])
    return (v[0] + w * tx + (uy * tz - uz * ty), v[1] + w * ty + (uz * tx - ux * tz), v[2] + w * tz + (ux * ty - uy * tx))


def qconj(q):
    return (-q[0], -q[1], -q[2], q[3])


def load(path):
    """bones: list of dicts {name, parent, q, t, Q, T} with Q, T the object (model space) rotation and position."""
    bones = []
    for line in open(path, encoding="latin-1"):
        p = line.split()
        if not p or p[0] != "bone":
            continue
        # Parsed from the end: some bones have no (or odd) names.
        q = tuple(map(float, p[-7:-3]))
        t = tuple(map(float, p[-3:]))
        bones.append({"name": " ".join(p[2:-8]) or f"#{p[1]}", "parent": int(p[-8]), "q": q, "t": t})
    for b in bones:
        if b["parent"] < 0:
            b["Q"], b["T"] = b["q"], b["t"]
        else:
            par = bones[b["parent"]]
            b["Q"] = qmul(par["Q"], b["q"])
            r = qrot(par["Q"], b["t"])
            b["T"] = (par["T"][0] + r[0], par["T"][1] + r[1], par["T"][2] + r[2])
    return bones


if __name__ == "__main__":
    bones = load(sys.argv[1])
    for b in bones:
        if len(sys.argv) < 3 or b["name"] in sys.argv[2:]:
            print(b["name"], [round(x, 3) for x in b["T"]])
