"""Lowest foot point per frame of a retargeted clip (stance feet should touch z = 0, none go far below).
usage (WSL): python foot_check.py <model.xml> <clip.npz>
"""
import sys

import mujoco
import numpy as np

m = mujoco.MjModel.from_xml_path(sys.argv[1])
d = mujoco.MjData(m)
q = np.load(sys.argv[2])["qpos"]
feet = {mujoco.mj_name2id(m, mujoco.mjtObj.mjOBJ_BODY, n) for n in ("l_foot", "r_foot", "l_toe", "r_toe")}
lows = []
for row in q:
    d.qpos[:] = row
    mujoco.mj_forward(m, d)
    z = [d.geom_xpos[g][2] - abs(d.geom_xmat[g][8]) * (m.geom_size[g][1] if m.geom_type[g] == mujoco.mjtGeom.mjGEOM_CAPSULE else 0) - m.geom_size[g][0]
         for g in range(m.ngeom) if m.geom_bodyid[g] in feet]
    lows.append(min(z))
lows = np.array(lows)
print(f"lowest foot point: min {lows.min():.3f} 5% {np.percentile(lows, 5):.3f} median {np.median(lows):.3f} max {lows.max():.3f}")
print("pelvis height: median %.3f" % np.median(q[:, 2]))
