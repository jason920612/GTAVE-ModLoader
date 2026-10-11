"""Trevor's physical body as a LocoMuJoCo humanoid, and our retargeted clips as LocoMuJoCo trajectories
(research/phase0.md §49). Runs in WSL's ~/loco-env (LocoMuJoCo 1.0, MuJoCo 3.2.7).

The body model is tools/make_mjcf.py's rigid-hands output with mimic sites and 2x strength
(examples/basketball/data/player_two_loco.xml). Observation: root pose without x/y, every hinge angle, root and hinge
velocities, as LocoMuJoCo's own humanoids. Actions: the motors (torque control).
"""
import os
from typing import List, Tuple

import jax.numpy as jnp
import mujoco
import numpy as np
from loco_mujoco.core import ObservationType
from loco_mujoco.core.utils import info_property
from loco_mujoco.environments.humanoids.base_robot_humanoid import BaseRobotHumanoid
from loco_mujoco.trajectory import Trajectory, TrajectoryData, TrajectoryInfo, TrajectoryModel
from mujoco import MjSpec

# LocoMuJoCo (source, with MuJoCo 3.15) compares joint types from JAX arrays with MuJoCo's enum, which is never equal,
# and rejects every trajectory it rebuilds internally; compare as plain integers.
from loco_mujoco.trajectory import dataclasses as _traj_dc  # noqa: E402

_post_init = _traj_dc.TrajectoryInfo.__post_init__


def _post_init_int_types(self):
    model = self.model
    if model is not None and not isinstance(model.jnt_type, np.ndarray):
        object.__setattr__(self, "model", model.replace(jnt_type=np.asarray(model.jnt_type))) if hasattr(model, "replace") else None
    types = [int(t) for t in np.asarray(self.model.jnt_type)]
    real = self.model
    try:
        object.__setattr__(self, "model", _IntTypes(real, types))
        _post_init(self)
    finally:
        object.__setattr__(self, "model", real)


class _IntTypes:
    def __init__(self, model, types):
        self._m, self.jnt_type = model, types

    def __getattr__(self, name):
        return getattr(self._m, name)


_traj_dc.TrajectoryInfo.__post_init__ = _post_init_int_types

# MJX 3.15 renamed make_data's contact capacity "nconmax" to "naconmax"; LocoMuJoCo still passes the old name.
from mujoco import mjx as _mjx  # noqa: E402

_make_data = _mjx.make_data


def _make_data_compat(*args, nconmax=None, **kwargs):
    if nconmax is not None and "naconmax" not in kwargs:
        kwargs["naconmax"] = nconmax
    return _make_data(*args, **kwargs)


_mjx.make_data = _make_data_compat

MODEL = os.path.join(os.path.dirname(__file__), "..", "..", "..", "examples", "basketball", "data", "player_two_loco.xml")


class TrevorBody(BaseRobotHumanoid):
    mjx_enabled = False

    def __init__(self, spec=None, observation_spec=None, actuation_spec=None, **kwargs):
        spec = mujoco.MjSpec.from_file(spec or MODEL) if not isinstance(spec, MjSpec) else spec
        hinges = [j.name for j in spec.joints if j.type == mujoco.mjtJoint.mjJNT_HINGE]
        if observation_spec is None:
            observation_spec = ([ObservationType.FreeJointPosNoXY("q_root", xml_name="root")]
                                + [ObservationType.JointPos(f"q_{j}", xml_name=j) for j in hinges]
                                + [ObservationType.FreeJointVel("dq_root", xml_name="root")]
                                + [ObservationType.JointVel(f"dq_{j}", xml_name=j) for j in hinges])
        else:
            observation_spec = self.parse_observation_spec(observation_spec)
        if actuation_spec is None:
            actuation_spec = [a.name for a in spec.actuators]
        super().__init__(spec=spec, actuation_spec=actuation_spec, observation_spec=observation_spec, **kwargs)

    @classmethod
    def get_default_xml_file_path(cls) -> str:
        return MODEL

    @info_property
    def upper_body_xml_name(self) -> str:
        return "chest"

    @info_property
    def root_free_joint_xml_name(self) -> str:
        return "root"

    @info_property
    def root_height_healthy_range(self) -> Tuple[float, float]:
        return (0.6, 1.5)


class MjxTrevorBody(TrevorBody):
    mjx_enabled = True

    def __init__(self, timestep=0.002, n_substeps=5, **kwargs):
        model_option_conf = kwargs.pop("model_option_conf", dict(iterations=4, ls_iterations=6,
                                                                  disableflags=mujoco.mjtDisableBit.mjDSBL_EULERDAMP))
        super().__init__(timestep=timestep, n_substeps=n_substeps, model_option_conf=model_option_conf, **kwargs)

    def _modify_spec_for_mjx(self, spec: MjSpec) -> MjSpec:
        # As LocoMuJoCo does for its robots: only the feet touch the floor (MJX is slow with many contacts).
        feet = {"l_foot", "r_foot", "l_toe", "r_toe"}

        def walk(body):
            yield body
            for child in body.bodies:
                yield from walk(child)

        for body in walk(spec.worldbody):
            for g in body.geoms:
                if body.name == "world" or g.name == "floor":
                    g.contype, g.conaffinity = 1, 0
                elif body.name in feet:
                    g.contype, g.conaffinity = 0, 1
                else:
                    g.contype, g.conaffinity = 0, 0
        return spec


TrevorBody.register()
MjxTrevorBody.register()


def trajectory(npz_path: str, model_path: str = MODEL) -> Trajectory:
    """One of our retargeted clips (qpos at its fps) as a LocoMuJoCo Trajectory (velocities by central differences)."""
    m = mujoco.MjModel.from_xml_path(model_path)
    clip = np.load(npz_path)
    q, fps = clip["qpos"], float(clip["fps"])
    qd = np.zeros((len(q), m.nv))
    for k in range(len(q)):
        a, b = q[max(k - 1, 0)], q[min(k + 1, len(q) - 1)]
        mujoco.mj_differentiatePos(m, qd[k], (min(k + 1, len(q) - 1) - max(k - 1, 0)) / fps, a, b)
    names = [mujoco.mj_id2name(m, mujoco.mjtObj.mjOBJ_JOINT, i) for i in range(m.njnt)]
    # NumPy joint types: a JAX scalar does not compare equal to MuJoCo's joint type enum (LocoMuJoCo checks that).
    info = TrajectoryInfo(names, model=TrajectoryModel(m.njnt, np.array(m.jnt_type)), frequency=fps)
    data = TrajectoryData(jnp.array(q[1:-1]), jnp.array(qd[1:-1]), split_points=jnp.array([0, len(q) - 2]))
    return Trajectory(info, data)
