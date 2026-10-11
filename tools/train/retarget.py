"""Retarget a CMU clip to the physical body (research/phase0.md §48).
usage (WSL): python retarget.py <model.xml> <subject.asf> <clip.amc> <out.npz> [fps = 30]

Bone lengths differ between the actor and the character, so poses are matched by segment directions rather than joint
positions: for every pair below, the unit vector between two body origins of the model should point the same way as
the vector between the two corresponding mocap joints. The pelvis position is the mocap root scaled by the ratio of leg
lengths, placed so that the midpoint of the hip joints matches. Twists that directions cannot fix (forearm, hips, ...) are pulled towards zero by a small weight.
Each frame is solved by least squares (joint limits as bounds) starting from the previous frame's answer; afterwards
the clip is lowered or raised frame by frame so the lower shoe touches the floor in every frame.
Output: qpos (frames x nq), fps, per-frame mean direction error (degrees).
"""
import sys

import mujoco
import numpy as np
from scipy.optimize import least_squares
from scipy.spatial.transform import Rotation

sys.path.insert(0, __import__("os").path.dirname(__file__))
from mocap import Asf, joints, read_amc  # noqa: E402

# Torso bodies follow the matching mocap bone's rotation (both rest poses stand upright facing the same way).
# Points inside one body (pelvis -> hips, chest -> shoulders) cannot change direction, and their rest directions
# differ between the two skeletons, so they are not matched as directions.
ORIENT = [("pelvis", "root", 3.0), ("abdomen", "lowerback", 1.5), ("chest", "thorax", 1.5), ("head", "head", 1.0)]
# Limbs: (model body from, model body to, mocap joint from, mocap joint to, weight).
PAIRS = []
for s in "lr":
    PAIRS += [(f"{s}_thigh", f"{s}_calf", f"{s}hipjoint", f"{s}femur", 2.0), (f"{s}_calf", f"{s}_foot", f"{s}femur", f"{s}tibia", 2.0),
              (f"{s}_foot", f"{s}_toe", f"{s}tibia", f"{s}foot", 1.0), (f"{s}_upperarm", f"{s}_forearm", f"{s}clavicle", f"{s}humerus", 1.5),
              (f"{s}_forearm", f"{s}_hand", f"{s}humerus", f"{s}radius", 1.5)]
TWIST = 0.05  # weight pulling every hinge towards zero (radians)


def main():
    model_path, asf_path, amc_path, out_path = sys.argv[1:5]
    fps = float(sys.argv[5]) if len(sys.argv) > 5 else 30.0
    m = mujoco.MjModel.from_xml_path(model_path)
    d = mujoco.MjData(m)
    asf = Asf(asf_path)
    frames = read_amc(amc_path)[:: max(1, round(120 / fps))]
    body = {mujoco.mj_id2name(m, mujoco.mjtObj.mjOBJ_BODY, i): i for i in range(m.nbody)}
    nh = m.nq - 7
    lo = np.concatenate([[-np.inf] * 6, m.jnt_range[1:, 0]])
    hi = np.concatenate([[np.inf] * 6, m.jnt_range[1:, 1]])

    # Leg length ratio (hip to ankle) from the rest pose of both.
    mujoco.mj_kinematics(m, d)
    leg_model = np.linalg.norm(d.xpos[body["l_thigh"]] - d.xpos[body["l_calf"]]) + np.linalg.norm(d.xpos[body["l_calf"]] - d.xpos[body["l_foot"]])
    p0, _ = joints(asf, frames[0])
    leg_mocap = np.linalg.norm(p0["lhipjoint"] - p0["lfemur"]) + np.linalg.norm(p0["lfemur"] - p0["ltibia"])
    scale = leg_model / leg_mocap
    print(f"{len(frames)} frames at {fps} fps; leg {leg_mocap:.3f} m -> {leg_model:.3f} m (x{scale:.3f})")

    def qpos_of(x):
        q = np.zeros(m.nq)
        q[:3] = x[:3]
        q[3:7] = Rotation.from_rotvec(x[3:6]).as_quat(scalar_first=True)
        q[7:] = x[6:]
        return q

    out, errors = [], []
    x = np.zeros(6 + nh)
    for k, frame in enumerate(frames):
        p, rots = joints(asf, frame)
        # The hips are the matching points of the two skeletons (root and pelvis origins sit at different heights).
        target_hips = (p["lhipjoint"] + p["rhipjoint"]) / 2 * scale
        dirs = [(b1, b2, (p[j2] - p[j1]) / (np.linalg.norm(p[j2] - p[j1]) or 1), w) for b1, b2, j1, j2, w in PAIRS]
        if k == 0:
            x[:3] = target_hips

        def residual(v):
            d.qpos[:] = qpos_of(v)
            mujoco.mj_kinematics(m, d)
            r = [3.0 * ((d.xpos[body["l_thigh"]] + d.xpos[body["r_thigh"]]) / 2 - target_hips)]
            for b, bone, w in ORIENT:
                xm = d.xmat[body[b]].reshape(3, 3)
                r.append(w * Rotation.from_matrix(rots[bone].T @ xm).as_rotvec())
            for b1, b2, t, w in dirs:
                u = d.xpos[body[b2]] - d.xpos[body[b1]]
                r.append(w * (u / np.linalg.norm(u) - t))
            r.append(TWIST * v[6:])
            return np.concatenate(r)

        # MuJoCo keeps joint ranges in radians after compiling.
        sol = least_squares(residual, np.clip(x, lo + 1e-6, hi - 1e-6), bounds=(lo, hi), max_nfev=200 if k else 2000)
        x = sol.x
        q = qpos_of(x)
        d.qpos[:] = q
        mujoco.mj_kinematics(m, d)
        err = [np.degrees(np.arccos(np.clip(((d.xpos[body[b2]] - d.xpos[body[b1]]) / np.linalg.norm(d.xpos[body[b2]] - d.xpos[body[b1]])) @ t, -1, 1))) for b1, b2, t, _ in dirs]
        errors.append(err)
        out.append(q)
    qpos = np.array(out)
    # Feet on the floor: the lowest geom point over the clip at z = 0 (per frame lowest foot capsule bottom).
    lows = []
    for q in qpos:
        d.qpos[:] = q
        mujoco.mj_kinematics(m, d)
        mujoco.mj_forward(m, d)
        # A capsule's lowest point: centre - |axis z| * half length - radius (spheres: half length 0).
        z = [d.geom_xpos[g][2] - abs(d.geom_xmat[g][8]) * (m.geom_size[g][1] if m.geom_type[g] == mujoco.mjtGeom.mjGEOM_CAPSULE else 0) - m.geom_size[g][0]
             for g in range(m.ngeom) if m.geom_bodyid[g] in (body["l_foot"], body["r_foot"], body["l_toe"], body["r_toe"])]
        lows.append(min(z))
    # Feet on the floor in every frame: the lower shoe touches z = 0 (walking and dribbling always have a foot down;
    # a windowed floor left the stance foot floating in most frames, so physics had nothing to stand on).
    # Lightly smoothed (3 frames) against jitter; the capture's own floor drifts, so this is done per frame.
    lows = np.array(lows)
    floor = np.convolve(np.pad(lows, 1, mode="edge"), np.ones(3) / 3, mode="valid")
    qpos[:, 2] -= floor
    errors = np.array(errors)
    names = [f"{b1}->{b2}" for b1, b2, *_ in PAIRS]
    print(f"direction error: mean {errors.mean():.1f} deg, 95% {np.percentile(errors, 95):.1f} deg")
    for i in np.argsort(-errors.mean(axis=0))[:5]:
        print(f"   worst {names[i]}: mean {errors[:, i].mean():.1f} deg, max {errors[:, i].max():.1f}")
    np.savez(out_path, qpos=qpos, fps=fps, errors=errors, names=names)


if __name__ == "__main__":
    main()
