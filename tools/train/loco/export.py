"""Export a trained LocoMuJoCo policy for the game (research/phase0.md §50).
usage (WSL, ~/loco-env): python -u export.py <run dir> <clip.npz> <out dir> [golden steps = 300]

Writes into <out dir>:
- body.mjb       the MuJoCo model exactly as trained (MJX variant: feet-only floor contacts, solver options, 2 ms step)
- policy.bin     actor network and observation normalisation (format below)
- reference.bin  the clip as the environment sees it: qpos/qvel per control step (100 Hz), start moved to the origin
- golden.bin     a CPU rollout with LocoMuJoCo itself: per step qpos, qvel, clip step, observation, action
                 (the game's implementation must reproduce observation and action from qpos/qvel/step)

Binary formats (little endian): header "BBPL" / "BBRF" / "BBGD", u32 version 1, then
- policy:    u32 n_layers, u32 obs_dim, f32 mean[obs_dim], f32 var[obs_dim], per layer u32 in, u32 out,
             f32 kernel[in*out] (row-major in x out), f32 bias[out]; hidden layers tanh, last linear.
- reference: u32 steps, u32 nq, u32 nv, f32 dt, f32 qpos[steps*nq], f32 qvel[steps*nv]
- golden:    u32 steps, u32 nq, u32 nv, u32 obs_dim, u32 act_dim, per step: u32 clip_step, f32 qpos[nq], f32 qvel[nv],
             f32 obs[obs_dim], f32 action[act_dim]
"""
import glob
import os
import struct
import sys

import jax
import mujoco
import numpy as np
from omegaconf import OmegaConf

sys.path.insert(0, os.path.dirname(__file__))
import trevor  # noqa: E402
from loco_mujoco import TaskFactory  # noqa: E402
from loco_mujoco.algorithms import PPOJax  # noqa: E402
from loco_mujoco.smpl.retargeting import extend_motion  # noqa: E402
from loco_mujoco.task_factories import CustomDatasetConf  # noqa: E402


def f32(a):
    return np.asarray(a, dtype="<f4").tobytes()


def main():
    run, clip, out = sys.argv[1], sys.argv[2], sys.argv[3]
    golden_steps = int(sys.argv[4]) if len(sys.argv) > 4 else 300
    os.makedirs(out, exist_ok=True)
    agent_conf, agent_state = PPOJax.load_agent(glob.glob(os.path.join(run, "*.pkl"))[0])
    config = agent_conf.config.experiment
    traj = extend_motion("TrevorBody", OmegaConf.create({}), trevor.trajectory(clip))

    # The MJX environment as trained: its compiled model and trajectory (interpolated, origin-shifted).
    params = OmegaConf.to_container(config.env_params, resolve=True)
    params["th_params"] = dict(random_start=False, fixed_start_conf=(0, 0))
    factory = TaskFactory.get_factory_cls(config.task_factory.name)
    mjx_env = factory.make(**params, custom_dataset_conf=CustomDatasetConf(traj=traj))
    model = mjx_env.get_model() if hasattr(mjx_env, "get_model") else mjx_env._model
    mujoco.mj_saveModel(model, os.path.join(out, "body.mjb"), None)
    data = mjx_env.th.traj.data
    rq, rv = np.asarray(data.qpos), np.asarray(data.qvel)
    with open(os.path.join(out, "reference.bin"), "wb") as f:
        f.write(b"BBRF" + struct.pack("<IIIIf", 1, len(rq), rq.shape[1], rv.shape[1], float(mjx_env.dt)) + f32(rq) + f32(rv))
    print(f"model: nq {model.nq} nv {model.nv} nu {model.nu}, timestep {model.opt.timestep}, control dt {mjx_env.dt}; "
          f"reference {len(rq)} steps")

    # Policy: actor (FullyConnectedNet_0) and the normalisation statistics.
    p = agent_state.train_state.params
    stats = agent_state.train_state.run_stats["RunningMeanStd_0"]
    layers = [p["FullyConnectedNet_0"][f"Dense_{i}"] for i in range(len(p["FullyConnectedNet_0"]))]
    mean, var = np.asarray(stats["mean"]), np.asarray(stats["var"])
    with open(os.path.join(out, "policy.bin"), "wb") as f:
        f.write(b"BBPL" + struct.pack("<III", 1, len(layers), len(mean)) + f32(mean) + f32(var))
        for layer in layers:
            k, b = np.asarray(layer["kernel"]), np.asarray(layer["bias"])
            f.write(struct.pack("<II", *k.shape) + f32(k) + f32(b))
    print("policy:", " -> ".join(str(np.asarray(l["kernel"]).shape) for l in layers))

    # Golden rollout on the CPU environment (numpy backend) with the same model options and contacts.
    # Same physics and control rate as trained: the MJX variant's timestep, substeps and solver options (the CPU
    # class's defaults differ), then its contacts.
    cpu_params = dict(params, env_name="TrevorBody", timestep=float(model.opt.timestep), n_substeps=int(round(mjx_env.dt / model.opt.timestep)),
                      model_option_conf=dict(iterations=int(model.opt.iterations), ls_iterations=int(model.opt.ls_iterations),
                                             disableflags=int(model.opt.disableflags)))
    cpu_env = factory.make(**cpu_params, custom_dataset_conf=CustomDatasetConf(traj=traj))
    cpu_model = cpu_env._model
    cpu_model.geom_contype[:], cpu_model.geom_conaffinity[:] = model.geom_contype, model.geom_conaffinity
    print(f"CPU environment: dt {cpu_env.dt}, timestep {cpu_model.opt.timestep}, iterations {cpu_model.opt.iterations}")
    ts = agent_state.train_state

    @jax.jit
    def policy(obs):
        y, _ = agent_conf.network.apply({"params": ts.params, "run_stats": ts.run_stats}, obs, mutable=["run_stats"])
        return y[0].mean()

    obs = cpu_env.reset()
    rows = []
    for _ in range(golden_steps):
        d = cpu_env._data
        step_no = int(np.asarray(cpu_env._additional_carry.traj_state.subtraj_step_no))
        action = np.asarray(policy(obs[None])).reshape(-1)
        rows.append((step_no, d.qpos.copy(), d.qvel.copy(), np.asarray(obs).reshape(-1).copy(), action.copy()))
        obs, reward, absorbing, done, info = cpu_env.step(action)
        if done:
            break
    with open(os.path.join(out, "golden.bin"), "wb") as f:
        f.write(b"BBGD" + struct.pack("<IIIIII", 1, len(rows), cpu_model.nq, cpu_model.nv, rows[0][3].size, rows[0][4].size))
        for step_no, q, v, o, a in rows:
            f.write(struct.pack("<I", step_no) + f32(q) + f32(v) + f32(o) + f32(a))
    print(f"golden: {len(rows)} steps (CPU environment {'fell' if len(rows) < golden_steps else 'kept going'}), "
          f"obs {rows[0][3].size}, action {rows[0][4].size}")


if __name__ == "__main__":
    main()
