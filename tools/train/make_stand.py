"""A 'stand still' reference clip: the model's rest pose with the shoes on the floor (control case for training).
usage (WSL): python make_stand.py <model.xml> <out.npz> [seconds = 3]"""
import sys

import mujoco
import numpy as np

m = mujoco.MjModel.from_xml_path(sys.argv[1])
d = mujoco.MjData(m)
mujoco.mj_forward(m, d)
feet = {mujoco.mj_name2id(m, mujoco.mjtObj.mjOBJ_BODY, n) for n in ("l_foot", "r_foot", "l_toe", "r_toe")}
low = min(d.geom_xpos[g][2] - abs(d.geom_xmat[g][8]) * (m.geom_size[g][1] if m.geom_type[g] == mujoco.mjtGeom.mjGEOM_CAPSULE else 0) - m.geom_size[g][0]
          for g in range(m.ngeom) if m.geom_bodyid[g] in feet)
q = m.qpos0.copy()
q[2] -= low
seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 3.0
frames = int(seconds * 30)
np.savez(sys.argv[2], qpos=np.tile(q, (frames, 1)), fps=30.0)
print(f"rest pose, lowest shoe point was {low:.3f} m; pelvis at {q[2]:.3f} m; {frames} frames")
