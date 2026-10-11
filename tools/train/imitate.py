"""Motion imitation for the physical body (DeepMimic-style, Peng et al. 2018), trained with PPO on MJX (research §48).
usage (WSL): python imitate.py train <model.xml> <clip.npz> <out dir> [timesteps = 50e6] [environments = 2048]
             python imitate.py eval <model.xml> <clip.npz> <out dir>     (rollout of the saved policy -> rollout.npz)

- Reference state initialisation: every episode starts at a random time of the clip, in the clip's pose and velocity.
- Action: per joint, a correction to the reference angle (radians, x0.5); position servos (kp = 3 x torque limit per
  radian, kv = 0.1 kp, torque limited to the motor's gear) track the target, integrated implicitly.
- Control at 31.25 Hz (8 physics steps of 4 ms).
- Observation: body positions and velocities relative to the pelvis in its heading frame, pelvis height and tilt,
  joint angles and velocities, and the reference's next two frames (joint angles, pelvis offset) and the clip phase.
- Reward (DeepMimic weights): 0.65 pose (joint angles), 0.1 joint velocities, 0.15 end effectors (hands, feet, head
  relative to the pelvis), 0.1 centre of mass (pelvis position); pose and velocity scales lowered for 37 joints.
- Episodes end when the pelvis drops below 0.55 m, the bodies are on average more than 0.5 m from the reference,
  or the clip ends.
- Contacts: only the feet against the floor (no self contact) for speed; fingers are rigid (--rigid-hands model).
"""
import functools
import json
import os
import sys
import time

import jax
import mujoco
import numpy as np
from brax.envs.base import Env, State
from brax.io import model as model_io
from brax.training.agents.ppo import networks as ppo_networks
from brax.training.agents.ppo import train as ppo
from jax import numpy as jp
from mujoco import mjx

# Brax 0.14 still calls these; JAX 0.11 removed them. One GPU: replicate = add a leading device axis.
if not hasattr(jax, "device_put_replicated") or True:
    def _replicated(x, devices):
        return jax.tree.map(lambda a: jax.device_put(jp.broadcast_to(a, (len(devices),) + jp.shape(a)), devices[0]), x)

    def _sharded(xs, devices):
        return jax.tree.map(lambda *a: jax.device_put(jp.stack(a), devices[0]), *xs)

    jax.device_put_replicated = _replicated
    jax.device_put_sharded = _sharded

CONTROL_STEPS = 8  # physics steps per control step
KP = 3.0  # servo stiffness, N m per radian per N m of torque limit (DeepMimic used about 2.5)
KV = 0.1  # damping / stiffness, seconds
STRENGTH = float(os.environ.get("BB_STRENGTH", "1"))  # torque limits x this (the gears were sized for a ~45 kg humanoid)
END_EFFECTORS = ("l_hand", "r_hand", "l_foot", "r_foot", "head")


def load_model(path):
    m = mujoco.MjModel.from_xml_path(path)
    # Only the feet touch the floor (geom 0): walking and dribbling stand on them, and a fall ends the episode anyway.
    feet = {mujoco.mj_name2id(m, mujoco.mjtObj.mjOBJ_BODY, n) for n in ("l_foot", "r_foot", "l_toe", "r_toe")}
    on = np.array([m.geom_bodyid[g] in feet for g in range(m.ngeom)])
    m.geom_contype[:] = 0
    m.geom_conaffinity[:] = np.where(on, 1, 0)
    m.geom_contype[0] = 1
    # Position servos instead of the model's motors: MuJoCo integrates their damping implicitly (implicitfast), which
    # stays stable for light segments where a PD computed once per step oscillates. Torque limit = the motor's gear.
    gear = m.actuator_gear[:, 0].copy() * STRENGTH
    kp, kv = gear * KP, gear * KP * KV
    m.actuator_gaintype[:] = mujoco.mjtGain.mjGAIN_FIXED
    m.actuator_gainprm[:, 0] = kp
    m.actuator_biastype[:] = mujoco.mjtBias.mjBIAS_AFFINE
    m.actuator_biasprm[:, :3] = np.stack([np.zeros_like(kp), -kp, -kv], axis=1)
    m.actuator_gear[:, 0] = 1
    m.actuator_forcelimited[:] = 1
    m.actuator_forcerange[:] = np.stack([-gear, gear], axis=1)
    m.actuator_ctrllimited[:] = 1
    m.actuator_ctrlrange[:] = m.jnt_range[m.actuator_trnid[:, 0]]
    m.opt.integrator = mujoco.mjtIntegrator.mjINT_IMPLICITFAST
    m.opt.solver = mujoco.mjtSolver.mjSOL_NEWTON
    m.opt.iterations = 1
    m.opt.ls_iterations = 4
    return m


def reference(clip_path, model, dt):
    """Reference qpos/qvel resampled at the control rate; qvel by finite differences (free joint: world linear and
    body-frame angular velocity)."""
    clip = np.load(clip_path)
    q, fps = clip["qpos"], float(clip["fps"])
    t_src = np.arange(len(q)) / fps
    t = np.arange(0, t_src[-1], dt)
    qr = np.stack([np.interp(t, t_src, q[:, i]) for i in range(q.shape[1])], axis=1)
    qr[:, 3:7] /= np.linalg.norm(qr[:, 3:7], axis=1, keepdims=True)
    qv = np.zeros((len(t), model.nv))
    for k in range(len(t)):
        a, b = qr[max(k - 1, 0)], qr[min(k + 1, len(t) - 1)]
        mujoco.mj_differentiatePos(model, qv[k], dt * (min(k + 1, len(t) - 1) - max(k - 1, 0)), a, b)
    return qr.astype(np.float32), qv.astype(np.float32)


class Imitate(Env):
    """The physics state is MJX data stepped directly (Brax's pipeline recomputes body transforms on every physics
    step, which made it 17x slower here); body velocities come from positions at the start and end of a control step."""

    def __init__(self, model_path, clip_path):
        m = load_model(model_path)
        self._m = m
        self._mx = mjx.put_model(m)
        self._dt = m.opt.timestep * CONTROL_STEPS
        self._ref_q, self._ref_qd = (jp.array(x) for x in reference(clip_path, m, self._dt))
        self._frames = self._ref_q.shape[0]
        self._joint_lo = jp.array(m.jnt_range[1:, 0])
        self._joint_hi = jp.array(m.jnt_range[1:, 1])
        names = [mujoco.mj_id2name(m, mujoco.mjtObj.mjOBJ_BODY, i) for i in range(m.nbody)]
        self._ee = jp.array([names.index(n) - 1 for n in END_EFFECTORS])  # body positions below exclude the world
        # Reference body positions (for the reward and termination), from MuJoCo forward kinematics.
        d = mujoco.MjData(m)
        xs = []
        for q in np.array(self._ref_q):
            d.qpos[:] = q
            mujoco.mj_kinematics(m, d)
            xs.append(d.xpos[1:].copy())
        self._ref_x = jp.array(np.array(xs, dtype=np.float32))

    @property
    def dt(self):
        return self._dt

    @property
    def observation_size(self):
        return int(self.reset(jax.random.PRNGKey(0)).obs.shape[-1])

    @property
    def action_size(self):
        return int(self._m.nu)

    @property
    def backend(self):
        return "mjx"

    def init_data(self, q, qd):
        d = mjx.make_data(self._mx).replace(qpos=q, qvel=qd)
        return mjx.forward(self._mx, d)

    # Pelvis heading frame: rotation about z that removes the pelvis yaw.
    @staticmethod
    def _heading(quat):
        w, x, y, z = quat
        yaw = jp.arctan2(2 * (w * z + x * y), 1 - 2 * (y * y + z * z))
        c, s = jp.cos(-yaw), jp.sin(-yaw)
        return jp.array([[c, -s, 0], [s, c, 0], [0, 0, 1.0]])

    def _obs(self, d, vel, frame):
        q, qd = d.qpos, d.qvel
        pos = d.xpos[1:]
        h = self._heading(q[3:7])
        root = pos[0]
        rel = (pos - root) @ h.T
        w, x, y, z = q[3:7]
        up = jp.array([2 * (x * z + w * y), 2 * (y * z - w * x), 1 - 2 * (x * x + y * y)])  # pelvis z axis in the world
        f1, f2 = jp.minimum(frame + 1, self._frames - 1), jp.minimum(frame + 2, self._frames - 1)
        ref = []
        for f in (f1, f2):
            ref += [self._ref_q[f, 7:] - q[7:], (self._ref_q[f, :3] - q[:3]) @ h.T]
        phase = frame / self._frames
        return jp.concatenate([rel.ravel(), (vel @ h.T).ravel(), jp.array([root[2]]), up, q[7:], qd[6:] * 0.1, *ref, jp.array([phase])])

    def reset(self, rng):
        frame = jax.random.randint(rng, (), 0, self._frames - 10)
        d = self.init_data(self._ref_q[frame], self._ref_qd[frame])
        vel = jp.zeros_like(d.xpos[1:])
        obs = self._obs(d, vel, frame)
        zero = jp.zeros(())
        metrics = {"pose": zero, "ee": zero, "com": zero, "body_error": zero}
        return State(d, obs, zero, zero, metrics, info={"frame": frame, "start": frame})

    def step(self, state, action):
        frame = state.info["frame"]
        target = jp.clip(self._ref_q[jp.minimum(frame + 1, self._frames - 1), 7:] + 0.5 * action, self._joint_lo, self._joint_hi)
        # Copy what is needed from the old state before stepping: keeping the old physics data alive after the scan
        # stops XLA from updating it in place (60x slower here).
        prev = state.pipeline_state.xpos[1:] + 0.0

        def substep(d, _):
            return mjx.step(self._mx, d), None

        d = jax.lax.scan(substep, state.pipeline_state.replace(ctrl=target), (), CONTROL_STEPS)[0]
        pos = d.xpos[1:]
        vel = (pos - prev) / self._dt
        frame = frame + 1
        f = jp.minimum(frame, self._frames - 1)
        rq, rqd, rx = self._ref_q[f], self._ref_qd[f], self._ref_x[f]
        # Pose includes the pelvis orientation (as DeepMimic's root joint): without it a whole-body forward lean costs
        # nothing and the policy topples forward while its joints still match.
        root_angle = 2 * jp.arccos(jp.clip(jp.abs(jp.dot(d.qpos[3:7], rq[3:7])), 0, 1))
        pose = jp.sum(jp.square(d.qpos[7:] - rq[7:])) + 3 * jp.square(root_angle)
        jvel = jp.sum(jp.square(d.qvel[6:] - rqd[6:]))
        ee = jp.sum(jp.square((pos[self._ee] - pos[0]) - (rx[self._ee] - rx[0])))
        com = jp.sum(jp.square(pos[0] - rx[0]))
        # DeepMimic's scales were for about 13 joints; with 37 hinges they leave no signal (0.2 rad everywhere -> ~0),
        # so pose and velocity are scaled down: 0.15 rad on every joint gives about half the pose reward.
        reward = 0.65 * jp.exp(-0.8 * pose) + 0.1 * jp.exp(-0.03 * jvel) + 0.15 * jp.exp(-40 * ee) + 0.1 * jp.exp(-10 * com)
        body_error = jp.mean(jp.linalg.norm(pos - rx, axis=1))
        bad = jp.any(jp.isnan(d.qpos))
        done = jp.where((pos[0, 2] < 0.55) | (body_error > 0.5) | (frame >= self._frames - 1) | bad, 1.0, 0.0)
        reward = jp.where(bad, 0.0, reward)
        state.metrics.update(pose=pose, ee=ee, com=com, body_error=body_error)
        # Brax's auto-reset puts back the episode's first physics state and observation when done, but not our clip
        # time: rewind it to the episode's start here so the next episode tracks the right frames.
        info = dict(state.info, frame=jp.where(done > 0, state.info["start"], frame))
        return state.replace(pipeline_state=d, obs=self._obs(d, vel, frame), reward=reward, done=done, info=info)


def train(model_path, clip_path, out, timesteps, num_envs):
    os.makedirs(out, exist_ok=True)
    env = Imitate(model_path, clip_path)
    log = open(os.path.join(out, "progress.jsonl"), "w")
    start = time.time()

    def progress(step, metrics):
        row = {"step": int(step), "time": round(time.time() - start, 1), **{k: float(v) for k, v in metrics.items() if np.ndim(v) == 0}}
        log.write(json.dumps(row) + "\n")
        log.flush()
        print(f"{row['time']:7.0f}s step {step:>11,} reward {row.get('eval/episode_reward', 0):8.2f} "
              f"length {row.get('eval/avg_episode_length', 0):6.1f} body error {row.get('eval/episode_body_error', 0):7.3f}", flush=True)

    make_policy, params, _ = ppo.train(
        environment=env, num_timesteps=int(timesteps), episode_length=env._frames, num_envs=num_envs, num_evals=20,
        learning_rate=3e-4, entropy_cost=1e-3, discounting=0.97, unroll_length=20, batch_size=512, num_minibatches=32,
        num_updates_per_batch=4, normalize_observations=True, reward_scaling=1.0, seed=0, progress_fn=progress,
        network_factory=functools.partial(ppo_networks.make_ppo_networks, policy_hidden_layer_sizes=(512, 256, 128)))
    model_io.save_params(os.path.join(out, "params.pkl"), params)
    print(f"saved {out}/params.pkl after {time.time() - start:.0f}s")


def evaluate(model_path, clip_path, out, start=0):
    env = Imitate(model_path, clip_path)
    params = model_io.load_params(os.path.join(out, "params.pkl"))
    networks = ppo_networks.make_ppo_networks(env.observation_size, env.action_size, policy_hidden_layer_sizes=(512, 256, 128))
    policy = jax.jit(ppo_networks.make_inference_fn(networks)(params, deterministic=True))
    step, reset = jax.jit(env.step), jax.jit(env.reset)
    state = reset(jax.random.PRNGKey(1))
    state = state.replace(pipeline_state=env.init_data(env._ref_q[start], env._ref_qd[start]), info={"frame": jp.array(start), "start": jp.array(start)})
    qs, errors = [], []
    for _ in range(env._frames - 1 - start):
        action, _ = policy(state.obs, jax.random.PRNGKey(0))
        state = step(state, action)
        qs.append(np.array(state.pipeline_state.qpos))
        errors.append(float(state.metrics["body_error"]))
        if float(state.done):
            break
    np.savez(os.path.join(out, f"rollout{start or ''}.npz"), qpos=np.array(qs), fps=1 / env.dt, errors=np.array(errors), start=start)
    print(f"rollout from frame {start}: {len(qs)} of {env._frames - 1 - start} steps, body error mean {np.mean(errors):.3f} m, max {np.max(errors):.3f} m")


if __name__ == "__main__":
    mode = sys.argv[1]
    if mode == "train":
        train(sys.argv[2], sys.argv[3], sys.argv[4], float(sys.argv[5]) if len(sys.argv) > 5 else 50e6, int(sys.argv[6]) if len(sys.argv) > 6 else 2048)
    else:
        evaluate(sys.argv[2], sys.argv[3], sys.argv[4], int(sys.argv[5]) if len(sys.argv) > 5 else 0)
