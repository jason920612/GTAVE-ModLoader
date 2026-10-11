"""Check the physical body model and measure simulation speed (basketball physics bodies, research/phase0.md §47).
usage (WSL, ~/bball-env): python bench.py <model.xml> [environments ...]

1. MuJoCo (CPU): contacts in the rest pose between bodies that are not parent and child (they would push the model
   apart on the first step), and a 2 s passive drop: the model must stay finite and come to rest on the floor.
2. MJX (GPU): steps per second for batches of environments (vmap over copies of the model's data).
"""
import sys
import time

import mujoco
import numpy as np


def check(model):
    data = mujoco.MjData(model)
    mujoco.mj_forward(model, data)
    pairs = []
    for i in range(data.ncon):
        c = data.contact[i]
        b1, b2 = model.geom_bodyid[c.geom1], model.geom_bodyid[c.geom2]
        if c.dist < -0.005:
            pairs.append((mujoco.mj_id2name(model, mujoco.mjtObj.mjOBJ_GEOM, c.geom1), mujoco.mj_id2name(model, mujoco.mjtObj.mjOBJ_GEOM, c.geom2),
                          round(float(c.dist), 3), b1, b2))
    print(f"rest pose: {data.ncon} contacts, {len(pairs)} penetrating more than 5 mm")
    for p in pairs[:30]:
        print("  ", p)
    for _ in range(int(2.0 / model.opt.timestep)):
        mujoco.mj_step(model, data)
    print(f"after 2 s passive: finite {np.isfinite(data.qpos).all()}, pelvis height {data.qpos[2]:.3f}, speed {np.abs(data.qvel).max():.3f}")
    return [(p[3], p[4]) for p in pairs]


def bench(model, counts):
    import jax
    from mujoco import mjx

    mx = mjx.put_model(model)
    print("device:", jax.devices()[0])
    for n in counts:
        data = jax.vmap(lambda _: mjx.make_data(mx))(np.arange(n))
        step = jax.jit(jax.vmap(mjx.step, in_axes=(None, 0)))
        data = step(mx, data)
        jax.block_until_ready(data)
        steps = 50
        t = time.perf_counter()
        for _ in range(steps):
            data = step(mx, data)
        jax.block_until_ready(data)
        dt = time.perf_counter() - t
        print(f"{n:6d} environments: {n * steps / dt:10.0f} steps/s ({n * steps / dt * model.opt.timestep:8.1f} simulated s per s)")


if __name__ == "__main__":
    m = mujoco.MjModel.from_xml_path(sys.argv[1])
    print(f"model: {m.nbody} bodies, {m.njnt} joints, {m.nv} dofs, {m.ngeom} geoms, {m.nu} actuators, timestep {m.opt.timestep}")
    check(m)
    counts = [int(x) for x in sys.argv[2:]] or [256, 1024, 4096]
    bench(m, counts)
