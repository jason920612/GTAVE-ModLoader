"""Raw physics speed for the imitation setup: MJX on the GPU (batched) and MuJoCo on the CPU (one thread), feet-only
floor contacts, Newton 1 iteration. usage (WSL): python -u raw_speed.py <model.xml>"""
import sys
import time

import jax
import mujoco
import numpy as np
from mujoco import mjx

sys.path.insert(0, __import__("os").path.dirname(__file__))
from imitate import load_model  # noqa: E402

m = load_model(sys.argv[1])
d = mujoco.MjData(m)
mujoco.mj_forward(m, d)
for timestep in (0.004, 0.008):
    m.opt.timestep = timestep
    d = mujoco.MjData(m)
    t = time.perf_counter()
    for _ in range(2000):
        mujoco.mj_step(m, d)
    cpu = 2000 / (time.perf_counter() - t)
    mx = mjx.put_model(m)
    for n in (4096,):
        data = jax.vmap(lambda _: mjx.make_data(mx))(np.arange(n))
        step = jax.jit(jax.vmap(mjx.step, in_axes=(None, 0)))
        data = step(mx, data)
        jax.block_until_ready(data)
        t = time.perf_counter()
        for _ in range(20):
            data = step(mx, data)
        jax.block_until_ready(data)
        gpu = n * 20 / (time.perf_counter() - t)
        print(f"timestep {timestep}: CPU one thread {cpu:8.0f} steps/s ({cpu * timestep:6.1f} sim s/s); GPU {n} envs {gpu:8.0f} steps/s ({gpu * timestep:6.1f} sim s/s)", flush=True)
