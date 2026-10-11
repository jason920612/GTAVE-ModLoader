"""Where MJX spends its time on the body model: compile time and step time, with and without contacts.
usage (WSL): python -u profile_step.py <model.xml> [environments]
"""
import sys
import time

import jax
import mujoco
import numpy as np
from mujoco import mjx


def run(model, n, label):
    t0 = time.perf_counter()
    mx = mjx.put_model(model)
    data = jax.vmap(lambda _: mjx.make_data(mx))(np.arange(n))
    step = jax.jit(jax.vmap(mjx.step, in_axes=(None, 0)))
    data = step(mx, data)
    jax.block_until_ready(data)
    compile_s = time.perf_counter() - t0
    t = time.perf_counter()
    for _ in range(20):
        data = step(mx, data)
    jax.block_until_ready(data)
    dt = (time.perf_counter() - t) / 20
    print(f"{label:28s} n={n:5d} compile {compile_s:7.1f}s  step {dt * 1000:8.2f} ms  -> {n / dt:9.0f} steps/s", flush=True)


if __name__ == "__main__":
    m = mujoco.MjModel.from_xml_path(sys.argv[1])
    n = int(sys.argv[2]) if len(sys.argv) > 2 else 64
    d = mujoco.MjData(m)
    mujoco.mj_forward(m, d)
    print(f"{m.nbody} bodies, {m.nv} dofs, {m.ngeom} geoms, solver {m.opt.solver}, iterations {m.opt.iterations}", flush=True)
    off = mujoco.MjModel.from_xml_path(sys.argv[1])
    off.opt.disableflags |= mujoco.mjtDisableBit.mjDSBL_CONTACT
    run(off, n, "no contacts")
    floor = mujoco.MjModel.from_xml_path(sys.argv[1])
    floor.geom_contype[1:] = 0  # bodies only touch the floor (conaffinity kept)
    floor.geom_conaffinity[0] = 1
    floor.geom_contype[0] = 1
    floor.geom_conaffinity[1:] = np.where(floor.geom_conaffinity[1:] > 0, 1, 0)
    run(floor, n, "floor contacts only")
    run(m, n, "all contacts")
