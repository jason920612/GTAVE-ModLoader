"""Build a MuJoCo model (MJCF) of a ped's physical body from its skeleton and fitted capsules (research/phase0.md §47).
usage: make_mjcf.py <skeleton.txt> <capsules.json> <out.xml> [body mass kg = 90] [--rigid-hands]

--rigid-hands: the finger capsules are fixed to the hand (no finger joints): 43 instead of 75 degrees of freedom and
a shallower tree, for training whole-body skills fast; fingers are posed separately.

- Bodies: one per segment of fit_capsules.py, at the rest (bind) position of the segment's bone, axes aligned with the
  model space (x: the character's left, y: back, z: up; the character faces -y). Capsules are moved from the bone's
  frame into the body frame.
- Joints: spherical joints are three hinges (x, y, z) as in MuJoCo's own humanoids, so every degree of freedom can
  have a position actuator; knees, elbows and finger joints are single hinges whose axis is set from the rest pose
  so that a positive angle is flexion (forearm forward, shank back, finger towards the palm).
- Masses: de Leva (1996) male segment fractions of the body mass; the hand's share is split between palm and fingers.
  The capsules give the shape (inertia), not the mass (their volume overlaps and includes loose clothes).
- Contacts: body capsules collide with the floor, the ball and other players (contype 1 / conaffinity 1); fingers only
  with the ball (contype 2, conaffinity 0; the ball gets conaffinity 3), which keeps the contact count down. Parent-child
  pairs are excluded by MuJoCo, the thighs explicitly (loose trousers overlap at rest).
"""
import json
import math
import sys

import numpy as np

sys.path.insert(0, __import__("os").path.dirname(__file__))
from skel_rest import load, qrot  # noqa: E402

FWD = np.array([0.0, -1.0, 0.0])
UP = np.array([0.0, 0.0, 1.0])

# segment: (parent segment, joint kind)
TREE = {"pelvis": (None, "free"), "abdomen": ("pelvis", "spine"), "chest": ("abdomen", "spine"), "head": ("chest", "neck")}
for s in "lr":
    TREE.update({f"{s}_upperarm": ("chest", "shoulder"), f"{s}_forearm": (f"{s}_upperarm", "elbow"), f"{s}_hand": (f"{s}_forearm", "wrist"),
                 f"{s}_thigh": ("pelvis", "hip"), f"{s}_calf": (f"{s}_thigh", "knee"), f"{s}_foot": (f"{s}_calf", "ankle"),
                 f"{s}_toe": (f"{s}_foot", "toe")})
    for f in range(5):
        for j in range(3):
            TREE[f"{s}_finger{f}{j}"] = (f"{s}_hand" if j == 0 else f"{s}_finger{f}{j - 1}", "thumb0" if (f, j) == (0, 0) else "finger")

# Joint limits, degrees: (name suffix, axis key, lo, hi). Axis keys: x/y/z model axes, "flex" (computed per segment).
JOINTS = {
    "spine": [("x", "x", -25, 40), ("y", "y", -25, 25), ("z", "z", -30, 30)],
    "neck": [("x", "x", -40, 50), ("y", "y", -35, 35), ("z", "z", -70, 70)],
    "shoulder": [("x", "x", -60, 170), ("y", "y", -90, 90), ("z", "z", -90, 90)],
    "elbow": [("flex", "flex", 0, 150), ("twist", "along", -80, 80)],
    "wrist": [("flex", "flex", -70, 70), ("dev", "side", -30, 30)],
    "hip": [("x", "x", -30, 120), ("y", "y", -40, 45), ("z", "z", -40, 40)],
    "knee": [("flex", "flex", 0, 150)],
    "ankle": [("flex", "x", -45, 30), ("inv", "y", -25, 25)],
    "toe": [("flex", "x", -30, 50)],
    "finger": [("flex", "flex", -10, 95)],
    "thumb0": [("flex", "flex", -20, 60), ("spread", "side", -30, 50)],
}
# de Leva 1996, male, fractions of body mass.
MASS = {"head": 0.0694, "chest": 0.1596, "abdomen": 0.1633, "pelvis": 0.1117, "upperarm": 0.0271, "forearm": 0.0162,
        "hand": 0.0061, "thigh": 0.1416, "calf": 0.0433, "foot": 0.0137}
GEAR = {"spine": 150, "neck": 40, "shoulder": 80, "elbow": 60, "wrist": 20, "hip": 200, "knee": 160, "ankle": 90,
        "toe": 20, "finger": 2, "thumb0": 3}


def fmt(v):
    return " ".join(f"{x:.5g}" for x in v)


def main():
    skel_path, caps_path, out_path = sys.argv[1:4]
    rigid = "--rigid-hands" in sys.argv
    args = [a for a in sys.argv[4:] if not a.startswith("--")]
    total = float(args[0]) if args else 90.0
    bones = load(skel_path)
    by_name = {b["name"]: b for b in bones}
    segs = json.load(open(caps_path))["segments"]

    def origin(seg):
        return np.array(by_name[segs[seg]["bone"]]["T"])

    def world_caps(seg):
        b = by_name[segs[seg]["bone"]]
        return [(np.array(qrot(b["Q"], c["a"])) + b["T"], np.array(qrot(b["Q"], c["b"])) + b["T"], c["radius"]) for c in segs[seg]["capsules"]]

    def child_dir(seg):
        """Rest direction of a segment: towards its child's origin, else along its capsule."""
        kids = [k for k, (p, _) in TREE.items() if p == seg and k in segs and "finger" not in k or (p == seg and seg.endswith(("0", "1")) and k in segs)]
        if kids:
            d = origin(kids[0]) - origin(seg)
        else:
            a, b, _ = world_caps(seg)[0]
            d = b - a if np.linalg.norm(b - a) > 1e-4 else b - origin(seg)
            if (b - origin(seg)) @ d < 0:
                d = -d
        return d / (np.linalg.norm(d) or 1)

    def palm_normal(side):
        hand = origin(f"{side}_hand")
        i, p = origin(f"{side}_finger10") - hand, origin(f"{side}_finger40") - hand
        n = np.cross(i, p)
        n /= np.linalg.norm(n)
        # The palm side is where the fingers curl to at rest.
        tip = world_caps(f"{side}_finger22")[0][1] - origin(f"{side}_finger20")
        return n if tip @ n > 0 else -n

    def axis(seg, key):
        if key in "xyz":
            return np.eye(3)["xyz".index(key)]
        d = child_dir(seg)
        side = seg[0]
        if key == "along":
            return d
        if seg.endswith("_calf"):
            towards = -FWD  # the shank flexes backwards
        elif "finger" in seg or seg.endswith("_hand"):
            towards = palm_normal(side)
        else:
            towards = FWD
        flex = np.cross(d, towards)
        flex /= np.linalg.norm(flex)
        return flex if key == "flex" else np.cross(flex, d) / np.linalg.norm(np.cross(flex, d))

    def mass(seg):
        name = seg.split("_", 1)[-1]
        if "finger" in seg:
            return total * MASS["hand"] * 0.4 / 15
        if name == "hand":
            return total * MASS["hand"] * 0.6
        if name == "toe":
            return total * MASS["foot"] * 0.2
        if name == "foot":
            return total * MASS["foot"] * 0.8
        return total * MASS[name]

    lines = ['<mujoco model="player_two">',
             '  <compiler angle="degree" autolimits="true"/>',
             '  <option timestep="0.004" iterations="4" ls_iterations="6"/>',
             '  <default>',
             '    <joint damping="2" armature="0.02" limited="true"/>',
             '    <geom type="capsule" contype="1" conaffinity="1" friction="0.9 0.02 0.002" condim="3"/>',
             '    <default class="finger"><joint damping="0.05" armature="0.0005"/><geom contype="2" conaffinity="0"/></default>',
             '  </default>',
             '  <worldbody>',
             '    <geom name="floor" type="plane" size="20 20 0.1" contype="1" conaffinity="1"/>',
             '    <light pos="0 0 6" dir="0 0 -1"/>']
    actuators = []

    def emit(seg, depth):
        parent, kind = TREE[seg]
        pos = origin(seg) - (origin(parent) if parent else np.array([0, 0, -1.02]))  # pelvis: feet on the floor
        cls = ' childclass="finger"' if "finger" in seg else ""
        ind = "    " + "  " * depth
        lines.append(f'{ind}<body name="{seg}" pos="{fmt(pos)}"{cls}>')
        if kind == "free":
            lines.append(f'{ind}  <freejoint name="root"/>')
        else:
            for suffix, key, lo, hi in JOINTS[kind]:
                name = f"{seg}_{suffix}"
                lines.append(f'{ind}  <joint name="{name}" type="hinge" axis="{fmt(axis(seg, key))}" range="{lo} {hi}"/>')
                actuators.append(f'    <motor name="{name}" joint="{name}" gear="{GEAR[kind]}" ctrlrange="-1 1"/>')
        def geoms(part, extra=""):
            caps = world_caps(part)
            m = mass(part) / len(caps)
            for k, (a, b, r) in enumerate(caps):
                a, b = a - origin(seg), b - origin(seg)
                if np.linalg.norm(b - a) < 1e-4:
                    lines.append(f'{ind}  <geom name="{part}{k}" type="sphere" pos="{fmt(a)}" size="{r:.4f}" mass="{m:.4f}"{extra}/>')
                else:
                    lines.append(f'{ind}  <geom name="{part}{k}" fromto="{fmt(a)} {fmt(b)}" size="{r:.4f}" mass="{m:.4f}"{extra}/>')

        geoms(seg)
        if rigid and seg.endswith("_hand"):
            for part in [k for k in TREE if k.startswith(seg[:2] + "finger") and k in segs]:
                geoms(part, ' contype="2" conaffinity="0"')
        for child in [k for k, (p, _) in TREE.items() if p == seg and k in segs and not (rigid and "finger" in k)]:
            emit(child, depth + 1)
        lines.append(f"{ind}</body>")

    emit("pelvis", 0)
    lines += ["  </worldbody>",
              # Loose trousers make the thighs overlap at rest.
              "  <contact>", '    <exclude body1="l_thigh" body2="r_thigh"/>', "  </contact>",
              "  <actuator>", *actuators, "  </actuator>", "</mujoco>"]
    open(out_path, "w").write("\n".join(lines) + "\n")
    print(f"{out_path}: {sum(1 for l in lines if '<body' in l)} bodies, {len(actuators)} actuated joints, "
          f"mass {sum(mass(s) for s in segs if s in TREE):.1f} kg")


if __name__ == "__main__":
    main()
