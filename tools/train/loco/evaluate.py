"""Deterministic rollout of a LocoMuJoCo-trained policy from the clip's first frame (research/phase0.md §49).
usage (WSL, ~/loco-env): python -u evaluate.py <run dir> <clip.npz> [steps = 1000]

Writes <run dir>/rollout.npz (qpos per control step, fps, steps until the episode ended) for viz_rollout.py
(which runs in ~/bball-env), and prints how long the body kept going.
"""
import glob
import os
import sys

import jax
import mujoco
import jax.numpy as jnp
import numpy as np
from omegaconf import OmegaConf

sys.path.insert(0, os.path.dirname(__file__))
import trevor  # noqa: E402,F401  (registers the environments, compatibility shims)
from loco_mujoco import TaskFactory  # noqa: E402
from loco_mujoco.algorithms import PPOJax  # noqa: E402
from loco_mujoco.smpl.retargeting import extend_motion  # noqa: E402
from loco_mujoco.task_factories import CustomDatasetConf  # noqa: E402


def find_qpos(state):
    """The physics data's qpos somewhere inside the (wrapped) environment state."""
    for path in ("data.qpos", "env_state.data.qpos", "env_state.env_state.data.qpos"):
        obj = state
        try:
            for part in path.split("."):
                obj = getattr(obj, part)
            return obj
        except AttributeError:
            continue
    leaves = jax.tree_util.tree_leaves_with_path(state)
    for path, leaf in leaves:
        if "qpos" in jax.tree_util.keystr(path):
            return leaf
    raise RuntimeError("no qpos in the environment state")


def main():
    run, clip = sys.argv[1], sys.argv[2]
    steps = int(sys.argv[3]) if len(sys.argv) > 3 else 1000
    agent_conf, agent_state = PPOJax.load_agent(glob.glob(os.path.join(run, "*.pkl"))[0])
    config = agent_conf.config.experiment
    traj = extend_motion("TrevorBody", OmegaConf.create({}), trevor.trajectory(clip))
    params = OmegaConf.to_container(config.env_params, resolve=True)
    params["th_params"] = dict(random_start=False, fixed_start_conf=(0, 0))
    env = TaskFactory.get_factory_cls(config.task_factory.name).make(**params, custom_dataset_conf=CustomDatasetConf(traj=traj))
    env = PPOJax._wrap_env(env, config)
    ts = agent_state.train_state
    ts.params["log_std"] = np.ones_like(ts.params["log_std"]) * -np.inf  # deterministic

    @jax.jit
    def act(ts, obs, rng):
        y, updates = agent_conf.network.apply({"params": ts.params, "run_stats": ts.run_stats}, obs, mutable=["run_stats"])
        return y[0].sample(seed=rng)

    nq = mujoco.MjModel.from_xml_path(trevor.MODEL).nq
    rng = jax.random.key(0)
    obs, state = env.reset(jax.random.split(rng, 2)[1:])
    step = jax.jit(env.step)
    qs, ended = [], None
    for i in range(steps):
        rng, k = jax.random.split(rng)
        obs, reward, absorbing, done, info, state = step(state, jnp.atleast_2d(act(ts, obs, k)))
        qs.append(np.array(find_qpos(state)).reshape(-1, nq)[0])
        if bool(np.array(done).any()) and ended is None:
            ended = i + 1
            break
    dt = float(env.dt) if hasattr(env, "dt") else 0.01
    np.savez(os.path.join(run, "rollout.npz"), qpos=np.array(qs), fps=1 / dt, ended=ended or steps)
    print(f"rollout: {len(qs)} steps ({len(qs) * dt:.2f} s); episode {'ended at step ' + str(ended) if ended else 'still going'}")


if __name__ == "__main__":
    main()
