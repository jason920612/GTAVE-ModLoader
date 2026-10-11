"""Do the actuators move the joints? qacc with ctrl 0 vs ctrl 1, CPU MuJoCo and MJX (WSL).
usage: python -u act_check.py <model.xml>"""
import sys

import mujoco
import numpy as np
from mujoco import mjx

sys.path.insert(0, __import__("os").path.dirname(__file__))
from imitate import load_model  # noqa: E402

m = load_model(sys.argv[1])
print("actuators", m.nu, "gear[0:5]", m.actuator_gear[:5, 0], "trntype", m.actuator_trntype[:3], "trnid", m.actuator_trnid[:3, 0],
      "ctrlrange", m.actuator_ctrlrange[0], "ctrllimited", m.actuator_ctrllimited[:3], "dyntype", m.actuator_dyntype[:3],
      "gaintype", m.actuator_gaintype[:3], "gainprm", m.actuator_gainprm[0, :3], "biastype", m.actuator_biastype[:3])
d = mujoco.MjData(m)
mujoco.mj_forward(m, d)
a0 = d.qacc.copy()
d.ctrl[:] = 1
mujoco.mj_forward(m, d)
print("CPU: max |qacc change|", np.abs(d.qacc - a0).max().round(2), "actuator force[0:5]", d.actuator_force[:5].round(1))
mx = mjx.put_model(m)
dx = mjx.forward(mx, mjx.make_data(mx))
dx1 = mjx.forward(mx, mjx.make_data(mx).replace(ctrl=np.ones(m.nu, np.float32)))
print("MJX: max |qacc change|", float(np.abs(np.array(dx1.qacc) - np.array(dx.qacc)).max()), "actuator force[0:5]", np.array(dx1.actuator_force[:5]).round(1))
