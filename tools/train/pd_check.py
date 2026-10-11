"""How long the body follows the reference with zero actions (pure PD on the reference), for several PD gains.
usage (WSL): python -u pd_check.py <model.xml> <clip.npz>

If even this falls within a fraction of a second, the policy has to learn balance from scratch; stronger gains make
the start easier (DeepMimic used stiff PD targets).
"""
import sys

import jax
import numpy as np
from jax import numpy as jp

sys.path.insert(0, __import__("os").path.dirname(__file__))
from imitate import Imitate  # noqa: E402

import imitate  # noqa: E402

for scale in (1.0, 3.0, 6.0):
    # A new environment per gain (jax.jit caches by function); the gain is read when the model is built.
    imitate.KP = scale
    env = Imitate(sys.argv[1], sys.argv[2])
    reset, step = jax.jit(jax.vmap(env.reset)), jax.jit(jax.vmap(env.step))
    n = 256
    s = reset(jax.random.split(jax.random.PRNGKey(0), n))
    alive = np.ones(n, bool)
    lengths = np.zeros(n)
    errs = []
    for k in range(env._frames):
        s = step(s, jp.zeros((n, env.action_size)))
        done = np.array(s.done) > 0
        lengths[alive] += 1
        errs.append(float(np.mean(np.array(s.metrics["body_error"])[alive])) if alive.any() else np.nan)
        alive &= ~done
        if not alive.any():
            break
    print(f"PD x{scale:<3}: steps before done: median {np.median(lengths):.0f} (of up to {env._frames}), "
          f"body error first 5 steps {np.round(errs[:5], 3)}", flush=True)
