"""Reference velocity vs finite differences, and where the body drifts in the first steps (WSL).
usage: python -u vel_check.py <model.xml> <clip.npz>"""
import sys

import jax
import numpy as np
from jax import numpy as jp

sys.path.insert(0, __import__("os").path.dirname(__file__))
from imitate import Imitate  # noqa: E402

env = Imitate(sys.argv[1], sys.argv[2])
q, qd, dt = np.array(env._ref_q), np.array(env._ref_qd), env.dt
k = 20
print("ref qvel[0:6] at k:", qd[k, :6].round(3))
print("pelvis finite difference:", ((q[k + 1, :3] - q[k - 1, :3]) / (2 * dt)).round(3))
s = env.reset(jax.random.PRNGKey(0))
s = s.replace(pipeline_state=env.init_data(env._ref_q[k], env._ref_qd[k]), info={"frame": jp.array(k)})
print("init qvel[0:6]:", np.array(s.pipeline_state.qvel[:6]).round(3))
step = jax.jit(env.step)
for i in range(14):
    s = step(s, jp.zeros(env.action_size))
    d = s.pipeline_state
    f = k + i + 1
    print(f"step {i + 1}: pelvis sim {np.array(d.qpos[:3]).round(3)} ref {q[f, :3].round(3)}; joint error {float(jp.abs(d.qpos[7:] - env._ref_q[f, 7:]).max()):.3f} rad; "
          f"body error {float(s.metrics['body_error']):.3f}; contacts {int((np.array(d.contact.dist) < 0.001).sum())}; "
          f"ctrl saturated {int((np.abs(np.array(d.ctrl)) > 0.99).sum())}/{env.action_size}")
