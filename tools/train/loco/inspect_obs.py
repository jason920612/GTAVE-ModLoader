"""What is in the policy's observation (LocoMuJoCo), and the network's shape (research §50).
usage (WSL, ~/loco-env): python -u inspect_obs.py <run dir> <clip.npz>"""
import glob
import os
import sys

import jax
import numpy as np
from omegaconf import OmegaConf

sys.path.insert(0, os.path.dirname(__file__))
import trevor  # noqa: E402,F401
from loco_mujoco import TaskFactory  # noqa: E402
from loco_mujoco.algorithms import PPOJax  # noqa: E402
from loco_mujoco.smpl.retargeting import extend_motion  # noqa: E402
from loco_mujoco.task_factories import CustomDatasetConf  # noqa: E402

run, clip = sys.argv[1], sys.argv[2]
agent_conf, agent_state = PPOJax.load_agent(glob.glob(os.path.join(run, "*.pkl"))[0])
config = agent_conf.config.experiment
traj = extend_motion("TrevorBody", OmegaConf.create({}), trevor.trajectory(clip))
params = OmegaConf.to_container(config.env_params, resolve=True)
env = TaskFactory.get_factory_cls(config.task_factory.name).make(**params, custom_dataset_conf=CustomDatasetConf(traj=traj))
print("env dt", env.dt, "obs size", env.info.observation_space.shape, "action size", env.info.action_space.shape)
oc = env.obs_container
for name, obs in oc.items():
    print(f"  {name:30s} {type(obs).__name__:28s} dim {getattr(obs, 'dim', '?')}")
print("network params:")
print(jax.tree_util.tree_map(lambda x: np.shape(x), agent_state.train_state.params))
print("run_stats:")
print(jax.tree_util.tree_map(lambda x: np.shape(x), agent_state.train_state.run_stats))
print("config wrappers: normalize_env", config.normalize_env, "len_obs_history", getattr(config, "len_obs_history", None))
