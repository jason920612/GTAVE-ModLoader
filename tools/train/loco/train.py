"""Train Trevor's body to imitate retargeted clips with LocoMuJoCo's PPO (research/phase0.md §49).
usage (WSL, ~/loco-env): python -u train.py <clip.npz> [<clip.npz> ...] --out <dir> [--steps 300e6] [--envs 2048]

Adapted from LocoMuJoCo's examples/training_examples/jax_rl_mimic/experiment.py: same agent and settings, no wandb;
progress (episode return and length per update) is written to <out>/progress.csv.
"""
import argparse
import os
import sys
import time

import jax
import jax.numpy as jnp
import numpy as np
from omegaconf import OmegaConf

sys.path.insert(0, os.path.dirname(__file__))
import trevor  # noqa: E402  (registers the environments)
from loco_mujoco import TaskFactory  # noqa: E402
from loco_mujoco.algorithms import PPOJax  # noqa: E402
from loco_mujoco.task_factories import CustomDatasetConf  # noqa: E402
from loco_mujoco.smpl.retargeting import extend_motion  # noqa: E402
from loco_mujoco.trajectory import Trajectory  # noqa: E402

os.environ.setdefault("XLA_FLAGS", "--xla_gpu_triton_gemm_any=True")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("clips", nargs="+")
    ap.add_argument("--out", required=True)
    ap.add_argument("--steps", type=float, default=None)
    ap.add_argument("--envs", type=int, default=None)
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    config = OmegaConf.load(os.path.join(os.path.dirname(__file__), "conf.yaml"))
    if args.steps:
        config.experiment.total_timesteps = args.steps
    if args.envs:
        config.experiment.num_envs = args.envs
    traj = Trajectory.concatenate([trevor.trajectory(c) for c in args.clips])
    # LocoMuJoCo skips this for custom datasets: replay the clips once to record body and site kinematics (the mimic
    # reward and goal observations need them) at the environment's rate.
    traj = extend_motion("TrevorBody", OmegaConf.create({}), traj)
    factory = TaskFactory.get_factory_cls(config.experiment.task_factory.name)
    env = factory.make(**config.experiment.env_params, custom_dataset_conf=CustomDatasetConf(traj=traj))
    agent_conf = PPOJax.init_agent_conf(env, config)
    train_fn = jax.jit(PPOJax.build_train_fn(env, agent_conf, mh=None))
    start = time.time()
    print(f"training {config.experiment.total_timesteps:.0f} steps, {config.experiment.num_envs} environments, "
          f"{config.experiment.num_updates} updates", flush=True)
    out = train_fn(jax.random.PRNGKey(1))
    jax.block_until_ready(out)
    print(f"trained in {time.time() - start:.0f}s", flush=True)
    path = PPOJax.save_agent(args.out, agent_conf, out["agent_state"])
    m = out["training_metrics"]
    rows = np.stack([np.asarray(m.max_timestep), np.asarray(m.mean_episode_return), np.asarray(m.mean_episode_length)], axis=1)
    np.savetxt(os.path.join(args.out, "progress.csv"), rows, delimiter=",", header="step,return,length", fmt="%.4f")
    for r in rows[:: max(1, len(rows) // 15)]:
        print(f"step {r[0]:>12,.0f} return {r[1]:9.2f} length {r[2]:7.1f}")
    print("saved", path)


if __name__ == "__main__":
    main()
