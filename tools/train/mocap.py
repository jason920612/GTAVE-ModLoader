"""CMU motion capture (ASF skeleton + AMC motion) and retargeting to the physical body (research/phase0.md §48).

ASF/AMC (Acclaim format, as documented with the CMU database):
- ASF: ":units" (length: file units per inch inverse, CMU uses 0.45), ":root" (order of the root's channels, axis order),
  ":bonedata" (per bone: direction (unit vector, global rest frame), length, axis (rx ry rz, degrees) with an order,
  dof (subset of rx ry rz)), ":hierarchy" (parent child...).
- AMC: per frame a number, then "bone values" lines; root: tx ty tz rx ry rz; others their dofs in degrees.
- Forward kinematics: a bone's rotation R = R_parent * C * M * C^-1, where C is the rotation of its axis angles and M of
  its dofs (both applied x then y then z, column vectors: Rz * Ry * Rx); its end = parent end + length * R * direction.
  CMU is Y up, the subject faces +Z; converted here to the model frame (Z up, facing -Y, X = the character's left):
  (x, y, z) -> (x, -z, y), and to metres: value / 0.45 inches.
"""
import numpy as np

INCH = 0.0254


def rot(axis, deg):
    a = np.deg2rad(deg)
    c, s = np.cos(a), np.sin(a)
    if axis == "x":
        return np.array([[1, 0, 0], [0, c, -s], [0, s, c]])
    if axis == "y":
        return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])
    return np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]])


def euler(values, order):
    """Rotation applying the axes in `order` (e.g. "XYZ": x first) to column vectors."""
    r = np.eye(3)
    for ax, v in zip(order.lower(), values):
        r = rot(ax, v) @ r
    return r


class Asf:
    def __init__(self, path):
        self.bones = {}
        self.children = {}
        self.unit = 0.45
        self.root_order = ["TX", "TY", "TZ", "RX", "RY", "RZ"]
        self.root_axis = "XYZ"
        section = None
        bone = None
        for raw in open(path):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            if line.startswith(":"):
                section = line.split()[0][1:]
                if section == "units":
                    continue
                continue
            words = line.split()
            if section == "units" and words[0] == "length":
                self.unit = float(words[1])
            elif section == "root":
                if words[0] == "order":
                    self.root_order = [w.upper() for w in words[1:]]
                elif words[0] == "axis":
                    self.root_axis = words[1]
            elif section == "bonedata":
                if words[0] == "begin":
                    bone = {"dof": []}
                elif words[0] == "end":
                    self.bones[bone["name"]] = bone
                elif words[0] == "name":
                    bone["name"] = words[1]
                elif words[0] == "direction":
                    bone["direction"] = np.array([float(w) for w in words[1:4]])
                elif words[0] == "length":
                    bone["length"] = float(words[1])
                elif words[0] == "axis":
                    bone["axis"] = euler([float(w) for w in words[1:4]], words[4])
                elif words[0] == "dof":
                    bone["dof"] = [w.lower() for w in words[1:]]
            elif section == "hierarchy" and words[0] not in ("begin", "end"):
                self.children[words[0]] = words[1:]

    def scale(self):
        return INCH / self.unit


def read_amc(path):
    frames, cur = [], None
    for raw in open(path):
        line = raw.strip()
        if not line or line.startswith(("#", ":")):
            continue
        words = line.split()
        if words[0].isdigit() and len(words) == 1:
            cur = {}
            frames.append(cur)
        elif cur is not None:
            cur[words[0]] = [float(w) for w in words[1:]]
    return frames


TO_MODEL = np.array([[1, 0, 0], [0, 0, -1], [0, 1, 0]], dtype=float)  # (x, y, z) -> (x, -z, y)


def joints(asf, frame):
    """End point of every bone (and "root") in model frame metres, and every bone's rotation from the rest pose
    (model frame; identity in the ASF rest pose, a T pose facing +Z = the model's -Y)."""
    s = asf.scale()
    root = frame["root"]
    ch = dict(zip(asf.root_order, root))
    pos = {"root": np.array([ch["TX"], ch["TY"], ch["TZ"]]) * s}
    rots = {"root": euler([ch["RX"], ch["RY"], ch["RZ"]], asf.root_axis)}

    def visit(parent):
        for child in asf.children.get(parent, []):
            b = asf.bones[child]
            values = frame.get(child, [])
            # AMC lists a bone's values in its "dof" order (e.g. ry rz); missing axes are 0.
            m = euler([values[b["dof"].index("r" + a)] if "r" + a in b["dof"] else 0.0 for a in "xyz"], "XYZ")
            c = b["axis"]
            rots[child] = rots[parent] @ c @ m @ c.T
            pos[child] = pos[parent] + b["length"] * s * (rots[child] @ b["direction"])
            visit(child)

    visit("root")
    return {k: TO_MODEL @ v for k, v in pos.items()}, {k: TO_MODEL @ r @ TO_MODEL.T for k, r in rots.items()}
