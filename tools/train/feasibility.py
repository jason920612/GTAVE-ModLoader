"""Is a reference clip physically possible for the body? Inverse dynamics per frame (research §48).
usage (WSL): python -u feasibility.py <model.xml> <clip.npz>

mj_inverse gives the generalised forces that would produce the clip's accelerations, with the floor contacts the
pose has. Forces on the free joint (pelvis) cannot come from muscles; a physically possible motion needs them
near zero (the feet must carry the body). They are reported relative to body weight, with the joint torques
relative to the torque limits. Following Yuan & Kitani 2020 (residual force control), large root forces mean the
reference itself is infeasible (foot sliding, floating feet, wrong mass distribution).
"""
import sys

import mujoco
import numpy as np

sys.path.insert(0, __import__("os").path.dirname(__file__))
from imitate import load_model, reference  # noqa: E402

m = load_model(sys.argv[1])
d = mujoco.MjData(m)
dt = m.opt.timestep * 8
q, qd = reference(sys.argv[2], m, dt)
weight = float(np.sum(m.body_mass)) * 9.81
qacc = np.zeros_like(qd)
qacc[1:-1] = (qd[2:] - qd[:-2]) / (2 * dt)
root_force, root_torque, joint_ratio, contacts = [], [], [], []
limits = m.actuator_forcerange[:, 1]
for k in range(1, len(q) - 1):
    d.qpos[:], d.qvel[:], d.qacc[:] = q[k], qd[k], qacc[k]
    mujoco.mj_inverse(m, d)
    f = d.qfrc_inverse
    root_force.append(np.linalg.norm(f[:3]) / weight)
    root_torque.append(np.linalg.norm(f[3:6]))
    joint_ratio.append(np.max(np.abs(f[6:]) / limits))
    contacts.append(d.ncon)
root_force, root_torque, joint_ratio = map(np.array, (root_force, root_torque, joint_ratio))
print(f"frames {len(root_force)}; floor contacts per frame: median {np.median(contacts):.0f}, frames with none {int(np.sum(np.array(contacts) == 0))}")
print(f"pelvis residual force / body weight: median {np.median(root_force):.2f}, 90% {np.percentile(root_force, 90):.2f}, max {root_force.max():.2f}")
print(f"pelvis residual torque (N m): median {np.median(root_torque):.0f}, 90% {np.percentile(root_torque, 90):.0f}")
print(f"largest joint torque / limit: median {np.median(joint_ratio):.2f}, 90% {np.percentile(joint_ratio, 90):.2f}")
