"""Reference vs simulated rollout as side-view stick figures (static check of a trained policy, research §48).
usage (WSL): python viz_rollout.py <model.xml> <clip.npz> <rollout.npz> <out.png> [frames = 10]

Top row: the reference (retargeted mocap) at the control rate; bottom row: the physics rollout of the policy, with
the mean body distance to the reference. The floor is the grey line.
"""
import sys

import matplotlib
import mujoco
import numpy as np

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

sys.path.insert(0, __import__("os").path.dirname(__file__))
from imitate import reference  # noqa: E402


def draw(ax, m, d, q, title):
    d.qpos[:] = q
    mujoco.mj_kinematics(m, d)
    for i in range(2, m.nbody):
        p = m.body_parentid[i]
        name = mujoco.mj_id2name(m, mujoco.mjtObj.mjOBJ_BODY, i)
        col = "tab:blue" if name.startswith("l_") else "tab:red" if name.startswith("r_") else "k"
        ax.plot([-d.xpos[p][1], -d.xpos[i][1]], [d.xpos[p][2], d.xpos[i][2]], color=col, lw=1.6)
    ax.axhline(0, color="0.6", lw=0.6)
    ax.set_title(title, fontsize=8)
    return d.xpos[1:].copy()


def main():
    model_path, clip_path, roll_path, out = sys.argv[1:5]
    count = int(sys.argv[5]) if len(sys.argv) > 5 else 10
    m = mujoco.MjModel.from_xml_path(model_path)
    d = mujoco.MjData(m)
    roll = np.load(roll_path)
    rq, dt = roll["qpos"], 1 / float(roll["fps"])
    ref, _ = reference(clip_path, m, dt)
    # LocoMuJoCo starts trajectories at the origin: line the rollout's start up with the reference horizontally.
    rq = rq.copy()
    rq[:, :2] += ref[min(1, len(ref) - 1), :2] - rq[0, :2]
    picks = np.linspace(0, len(rq) - 1, count).astype(int)
    fig, axes = plt.subplots(2, count, figsize=(2.1 * count, 6.5), sharey=True)
    for c, k in enumerate(picks):
        a = draw(axes[0, c], m, d, ref[min(k + 1, len(ref) - 1)], f"reference {(k + 1) * dt:.2f}s")
        b = draw(axes[1, c], m, d, rq[k], f"simulated {(k + 1) * dt:.2f}s")
        axes[1, c].set_xlabel(f"error {np.mean(np.linalg.norm(a - b, axis=1)) * 100:.1f} cm", fontsize=8)
        for ax in axes[:, c]:
            cx = -ref[min(k + 1, len(ref) - 1)][1]
            ax.set_xlim(cx - 0.8, cx + 0.8)
            ax.set_aspect("equal")
    axes[0, 0].set_ylim(-0.1, 1.9)
    plt.tight_layout()
    plt.savefig(out, dpi=90)


if __name__ == "__main__":
    main()
