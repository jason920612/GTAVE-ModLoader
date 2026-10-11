"""Control steps per second of the imitation environment (WSL). usage: python -u env_speed.py <model.xml> <clip.npz> <n>"""
import sys
import time

import jax
from jax import numpy as jp

sys.path.insert(0, __import__("os").path.dirname(__file__))
from imitate import Imitate  # noqa: E402

env = Imitate(sys.argv[1], sys.argv[2])
n = int(sys.argv[3])
reset = jax.jit(jax.vmap(env.reset))
step = jax.jit(jax.vmap(env.step))
t = time.perf_counter()
state = reset(jax.random.split(jax.random.PRNGKey(0), n))
# Twice: the first step changes the state's types (weak-typed scalars from reset), which compiles a second time.
for _ in range(2):
    state = step(state, jp.zeros((n, env.action_size)))
jax.block_until_ready(state)
print(f"compile {time.perf_counter() - t:.1f}s, obs {env.observation_size}, actions {env.action_size}", flush=True)
t = time.perf_counter()
for _ in range(10):
    state = step(state, jp.zeros((n, env.action_size)))
jax.block_until_ready(state)
dt = (time.perf_counter() - t) / 10
print(f"{n} envs: {dt * 1000:.1f} ms per control step -> {n / dt:.0f} control steps/s", flush=True)
