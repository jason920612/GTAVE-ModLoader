"""Stick figures of a retargeted clip next to the mocap it came from (static check, research/phase0.md §48).
usage (WSL): python viz_clip.py <model.xml> <subject.asf> <clip.amc> <retargeted.npz> <out.png> [frames = 8]

Top row: the mocap skeleton (scaled like the pelvis); bottom row: the model's body origins. Side view (y forward
to the left, z up) and a strip of evenly spaced frames, so walking or dribbling shows as it would in a flip book.
"""
import sys

import matplotlib
import mujoco
import numpy as np

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

sys.path.insert(0, __import__("os").path.dirname(__file__))
from mocap import Asf, joints, read_amc  # noqa: E402

MOCAP_LINKS = [("root", "lhipjoint"), ("lhipjoint", "lfemur"), ("lfemur", "ltibia"), ("ltibia", "lfoot"), ("root", "rhipjoint"),
               ("rhipjoint", "rfemur"), ("rfemur", "rtibia"), ("rtibia", "rfoot"), ("root", "lowerback"), ("lowerback", "upperback"),
               ("upperback", "thorax"), ("thorax", "upperneck"), ("upperneck", "head"), ("thorax", "lclavicle"), ("lclavicle", "lhumerus"),
               ("lhumerus", "lradius"), ("thorax", "rclavicle"), ("rclavicle", "rhumerus"), ("rhumerus", "rradius")]


def main():
    model_path, asf_path, amc_path, npz_path, out = sys.argv[1:6]
    count = int(sys.argv[6]) if len(sys.argv) > 6 else 8
    m = mujoco.MjModel.from_xml_path(model_path)
    d = mujoco.MjData(m)
    clip = np.load(npz_path)
    qpos, fps = clip["qpos"], float(clip["fps"])
    asf = Asf(asf_path)
    frames = read_amc(amc_path)[:: max(1, round(120 / fps))]
    picks = np.linspace(0, len(qpos) - 1, count).astype(int)
    fig, axes = plt.subplots(2, count, figsize=(2.2 * count, 7), sharey=True)
    for c, k in enumerate(picks):
        p, _ = joints(asf, frames[k])
        ax = axes[0, c]
        for a, b in MOCAP_LINKS:
            col = "tab:blue" if a.startswith("l") or b.startswith("l") else "tab:red" if a.startswith("r") or b.startswith("r") else "k"
            ax.plot([-p[a][1], -p[b][1]], [p[a][2], p[b][2]], color=col, lw=1.5)
        ax.set_title(f"mocap {k / fps:.2f}s", fontsize=8)
        d.qpos[:] = qpos[k]
        mujoco.mj_kinematics(m, d)
        ax = axes[1, c]
        for i in range(1, m.nbody):
            par = m.body_parentid[i]
            if par == 0:
                continue
            name = mujoco.mj_id2name(m, mujoco.mjtObj.mjOBJ_BODY, i)
            col = "tab:blue" if name.startswith("l_") else "tab:red" if name.startswith("r_") else "k"
            ax.plot([-d.xpos[par][1], -d.xpos[i][1]], [d.xpos[par][2], d.xpos[i][2]], color=col, lw=1.5)
        ax.axhline(0, color="0.6", lw=0.5)
        ax.set_title(f"model {k / fps:.2f}s", fontsize=8)
        for a in axes[:, c]:
            a.set_aspect("equal")
            cx = -qpos[k][1]
            a.set_xlim(cx - 0.8, cx + 0.8)
    axes[0, 0].set_ylim(-0.1, 1.9)
    plt.tight_layout()
    plt.savefig(out, dpi=90)


if __name__ == "__main__":
    main()
